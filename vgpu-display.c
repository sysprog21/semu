#include <stdio.h>
#include <stdlib.h>

#include "device.h"
#include "vgpu-display.h"

/* 'PRIMARY_SET'/'CURSOR_SET' own CPU-frame snapshots, so each queued command
 * can retain significantly more memory than an input event. Keep this backlog
 * deliberately small: display updates are lossy and quickly become stale, and
 * the emulator thread must be able to drop them rather than accumulate a large
 * queue of old frames.
 */
#define VGPU_DISPLAY_CMD_QUEUE_SIZE 64U
#define VGPU_DISPLAY_CMD_QUEUE_MASK (VGPU_DISPLAY_CMD_QUEUE_SIZE - 1U)

/* Reliable state for plane clear/removal events. The producer advances
 * 'generation' when the guest detaches a plane. The SDL consumer mirrors the
 * last applied value in 'consumed_generation'. Frame payloads remain in the
 * lossy SPSC queue below.
 */
struct vgpu_display_plane_clear_state {
    uint32_t generation;
    uint32_t consumed_generation;
};

static struct vgpu_display_plane_clear_state
    vgpu_display_primary_clear[VIRTIO_GPU_MAX_SCANOUTS];
static struct vgpu_display_plane_clear_state
    vgpu_display_cursor_clear[VIRTIO_GPU_MAX_SCANOUTS];
static uint32_t vgpu_display_scanout_count = 1U;

/* The SPSC queue carries lossy frame/move commands. It's process-wide and
 * currently assumes one 'virtio-gpu' producer. The GPU backend is the only
 * producer and the window backend is the only consumer. Commands entering this
 * bridge carry 'scanout_id' values already validated by the guest-facing
 * backend; the SDL consumer relies on that internal contract.
 */
static struct vgpu_display_cmd
    vgpu_display_cmd_queue[VGPU_DISPLAY_CMD_QUEUE_SIZE];
static uint32_t vgpu_display_cmd_head;
static uint32_t vgpu_display_cmd_tail;

static bool vgpu_display_unavailable;

static bool vgpu_display_is_cmd_stale(const struct vgpu_display_cmd *cmd)
{
    switch (cmd->type) {
    case VGPU_DISPLAY_CMD_PRIMARY_SET:
        return cmd->generation !=
               __atomic_load_n(
                   &vgpu_display_primary_clear[cmd->scanout_id].generation,
                   __ATOMIC_ACQUIRE);
    case VGPU_DISPLAY_CMD_CURSOR_SET:
    case VGPU_DISPLAY_CMD_CURSOR_MOVE:
        return cmd->generation !=
               __atomic_load_n(
                   &vgpu_display_cursor_clear[cmd->scanout_id].generation,
                   __ATOMIC_ACQUIRE);
    default:
        return false;
    }
}

static bool vgpu_display_pop_pending_clear_cmd(
    struct vgpu_display_plane_clear_state *states,
    enum vgpu_display_cmd_type type,
    struct vgpu_display_cmd *cmd)
{
    uint32_t scanout_count =
        __atomic_load_n(&vgpu_display_scanout_count, __ATOMIC_ACQUIRE);

    for (uint32_t i = 0; i < scanout_count; i++) {
        struct vgpu_display_plane_clear_state *state = &states[i];
        uint32_t generation =
            __atomic_load_n(&state->generation, __ATOMIC_ACQUIRE);

        if (state->consumed_generation == generation)
            continue;

        state->consumed_generation = generation;

        *cmd = (struct vgpu_display_cmd) {
            .type = type,
            .scanout_id = i,
            .generation = generation,
        };
        return true;
    }

    return false;
}

void vgpu_display_set_scanout_count(uint32_t scanout_count)
{
    if (scanout_count > VIRTIO_GPU_MAX_SCANOUTS)
        scanout_count = VIRTIO_GPU_MAX_SCANOUTS;

    __atomic_store_n(&vgpu_display_scanout_count, scanout_count,
                     __ATOMIC_RELEASE);
}

void vgpu_display_publish_primary_clear(uint32_t scanout_id)
{
    if (__atomic_load_n(&vgpu_display_unavailable, __ATOMIC_ACQUIRE))
        return;

    __atomic_add_fetch(&vgpu_display_primary_clear[scanout_id].generation, 1U,
                       __ATOMIC_ACQ_REL);
}

void vgpu_display_publish_cursor_clear(uint32_t scanout_id)
{
    if (__atomic_load_n(&vgpu_display_unavailable, __ATOMIC_ACQUIRE))
        return;

    __atomic_add_fetch(&vgpu_display_cursor_clear[scanout_id].generation, 1U,
                       __ATOMIC_ACQ_REL);
}

static bool vgpu_display_is_cmd_queue_full(void)
{
    uint32_t head = __atomic_load_n(&vgpu_display_cmd_head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&vgpu_display_cmd_tail, __ATOMIC_ACQUIRE);
    uint32_t next = (head + 1U) & VGPU_DISPLAY_CMD_QUEUE_MASK;
    return next == tail;
}

static void vgpu_display_push_cmd(struct vgpu_display_cmd *cmd)
{
    uint32_t head = __atomic_load_n(&vgpu_display_cmd_head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&vgpu_display_cmd_tail, __ATOMIC_ACQUIRE);
    uint32_t next = (head + 1U) & VGPU_DISPLAY_CMD_QUEUE_MASK;

    /* Keep the producer non-blocking. If the window backend falls behind,
     * prefer dropping lossy display updates over stalling guest/device
     * execution on the emulator thread. Clear commands do not use this queue.
     */
    if (next == tail) {
        vgpu_display_release_cmd(cmd);
        return;
    }

    vgpu_display_cmd_queue[head] = *cmd;
    __atomic_store_n(&vgpu_display_cmd_head, next, __ATOMIC_RELEASE);
}

static bool vgpu_display_pop_queued_cmd(struct vgpu_display_cmd *cmd)
{
    uint32_t tail = __atomic_load_n(&vgpu_display_cmd_tail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&vgpu_display_cmd_head, __ATOMIC_ACQUIRE);

    if (tail == head)
        return false;

    *cmd = vgpu_display_cmd_queue[tail];
    __atomic_store_n(&vgpu_display_cmd_tail,
                     (tail + 1U) & VGPU_DISPLAY_CMD_QUEUE_MASK,
                     __ATOMIC_RELEASE);
    return true;
}

void vgpu_display_release_cmd(struct vgpu_display_cmd *cmd)
{
    switch (cmd->type) {
    case VGPU_DISPLAY_CMD_PRIMARY_SET:
        free(cmd->u.primary_set.payload);
        break;
    case VGPU_DISPLAY_CMD_CURSOR_SET:
        free(cmd->u.cursor_set.payload);
        break;
    default:
        break;
    }
}

bool vgpu_display_pop_cmd(struct vgpu_display_cmd *cmd)
{
    /* Return true when '*cmd' is filled with a clear command or a valid queued
     * frame/move command. Stale queued commands are released and skipped;
     * return false only when no command remains.
     */
    for (;;) {
        /* Check clear command for primary and cursor plane. */
        if (vgpu_display_pop_pending_clear_cmd(vgpu_display_primary_clear,
                                               VGPU_DISPLAY_CMD_PRIMARY_CLEAR,
                                               cmd))
            return true;
        if (vgpu_display_pop_pending_clear_cmd(
                vgpu_display_cursor_clear, VGPU_DISPLAY_CMD_CURSOR_CLEAR, cmd))
            return true;

        /* Pop the command and check if it is still valid. */
        if (!vgpu_display_pop_queued_cmd(cmd))
            return false;
        if (!vgpu_display_is_cmd_stale(cmd))
            return true;

        /* Drop invalid command and continue. */
        vgpu_display_release_cmd(cmd);
    }
}

void vgpu_display_set_unavailable(void)
{
    struct vgpu_display_cmd cmd;

    /* This is an init-only fallback path for 'window-sw' initialization
     * failure, before the emulator thread starts publishing display commands.
     * It is not a concurrent shutdown primitive: a producer could otherwise
     * observe 'vgpu_display_unavailable == false', race with this drain, and
     * enqueue a payload after the queue was already drained.
     *
     * Still publish the latch atomically so later call sites keep the same
     * one-way handoff rule.
     */
    __atomic_store_n(&vgpu_display_unavailable, true, __ATOMIC_RELEASE);

    while (vgpu_display_pop_cmd(&cmd))
        vgpu_display_release_cmd(&cmd);
}

bool vgpu_display_can_publish(void)
{
    return !__atomic_load_n(&vgpu_display_unavailable, __ATOMIC_ACQUIRE) &&
           !vgpu_display_is_cmd_queue_full();
}

void vgpu_display_publish_primary_set(uint32_t scanout_id,
                                      struct vgpu_display_payload *payload)
{
    if (__atomic_load_n(&vgpu_display_unavailable, __ATOMIC_ACQUIRE)) {
        free(payload);
        return;
    }

    struct vgpu_display_cmd cmd = {
        .type = VGPU_DISPLAY_CMD_PRIMARY_SET,
        .scanout_id = scanout_id,
        .generation =
            __atomic_load_n(&vgpu_display_primary_clear[scanout_id].generation,
                            __ATOMIC_ACQUIRE),
        .u.primary_set = {.payload = payload},
    };
    vgpu_display_push_cmd(&cmd);
}

void vgpu_display_publish_cursor_set(uint32_t scanout_id,
                                     struct vgpu_display_payload *payload,
                                     int32_t x,
                                     int32_t y,
                                     uint32_t hot_x,
                                     uint32_t hot_y)
{
    if (__atomic_load_n(&vgpu_display_unavailable, __ATOMIC_ACQUIRE)) {
        free(payload);
        return;
    }

    struct vgpu_display_cmd cmd = {
        .type = VGPU_DISPLAY_CMD_CURSOR_SET,
        .scanout_id = scanout_id,
        .generation =
            __atomic_load_n(&vgpu_display_cursor_clear[scanout_id].generation,
                            __ATOMIC_ACQUIRE),
        .u.cursor_set = {.payload = payload,
                         .x = x,
                         .y = y,
                         .hot_x = hot_x,
                         .hot_y = hot_y},
    };
    vgpu_display_push_cmd(&cmd);
}

void vgpu_display_publish_cursor_move(uint32_t scanout_id, int32_t x, int32_t y)
{
    if (__atomic_load_n(&vgpu_display_unavailable, __ATOMIC_ACQUIRE))
        return;

    struct vgpu_display_cmd cmd = {
        .type = VGPU_DISPLAY_CMD_CURSOR_MOVE,
        .scanout_id = scanout_id,
        .generation =
            __atomic_load_n(&vgpu_display_cursor_clear[scanout_id].generation,
                            __ATOMIC_ACQUIRE),
        .u.cursor_move = {.x = x, .y = y},
    };
    vgpu_display_push_cmd(&cmd);
}
