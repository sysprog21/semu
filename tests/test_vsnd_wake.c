/* Unit test for virtio-snd TX worker wake-pipe notifications. */

#undef NDEBUG
#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "device.h"
#include "riscv.h"
#include "riscv_private.h"
#include "virtio.h"

enum {
    TEST_VSND_QUEUE_CTRL = 0,
    TEST_VSND_QUEUE_TX = 2,
    TEST_VSND_VQ_LEN = 8,
    TEST_VRING_AVAIL_F_NO_INTERRUPT = 1,
    TEST_VSND_R_PCM_SET_PARAMS = 0x0101,
    TEST_VSND_R_PCM_PREPARE = 0x0102,
    TEST_VSND_R_PCM_RELEASE = 0x0103,
    TEST_VSND_S_OK = 0x8000,
    TEST_VSND_FMT_S16 = 5,
    TEST_VSND_RATE_48000 = 6,
};

enum {
    ADDR_CTRL_DESC = 0x1000,
    ADDR_CTRL_AVAIL = 0x2000,
    ADDR_CTRL_USED = 0x3000,
    ADDR_TX_DESC = 0x10000,
    ADDR_TX_AVAIL = 0x12000,
    ADDR_TX_USED = 0x13000,
    ADDR_CTRL_REQ = 0x20000,
    ADDR_CTRL_RESP = 0x21000,
    ADDR_TX_XFER = 0x30000,
    ADDR_TX_PCM = 0x31000,
    ADDR_TX_STATUS = 0x32000,
    TEST_RAM_BYTES = 1024 * 1024,
};

void vm_set_exception(hart_t *vm, uint32_t cause, uint32_t tval)
{
    (void) vm;
    (void) cause;
    (void) tval;
}

static void wreg(virtio_snd_state_t *vsnd, uint32_t reg, uint32_t val)
{
    hart_t dummy = {0};
    virtio_snd_write(&dummy, vsnd, reg << 2, RV_MEM_SW, val);
}

static uint32_t rreg(virtio_snd_state_t *vsnd, uint32_t reg)
{
    hart_t dummy = {0};
    uint32_t val = 0;
    virtio_snd_read(&dummy, vsnd, reg << 2, RV_MEM_LW, &val);
    return val;
}

static void setup_vq(virtio_snd_state_t *vsnd,
                     uint32_t queue_idx,
                     uint32_t queue_num,
                     uint32_t desc_addr,
                     uint32_t avail_addr,
                     uint32_t used_addr)
{
    wreg(vsnd, VIRTIO_QueueSel, queue_idx);
    wreg(vsnd, VIRTIO_QueueNum, queue_num);
    wreg(vsnd, VIRTIO_QueueDescLow, desc_addr);
    wreg(vsnd, VIRTIO_QueueDriverLow, avail_addr);
    wreg(vsnd, VIRTIO_QueueDeviceLow, used_addr);
    wreg(vsnd, VIRTIO_QueueReady, 1);
}

static void send_ctrl_cmd(virtio_snd_state_t *vsnd,
                          uint8_t *ram,
                          uint16_t *ctrl_idx,
                          uint32_t req_len)
{
    struct virtq_desc *cd = (struct virtq_desc *) (ram + ADDR_CTRL_DESC);
    cd[0] = (struct virtq_desc) {ADDR_CTRL_REQ, req_len, VIRTIO_DESC_F_NEXT, 1};
    cd[1] = (struct virtq_desc) {ADDR_CTRL_RESP, 4, VIRTIO_DESC_F_WRITE, 0};

    *(uint32_t *) (ram + ADDR_CTRL_RESP) = 0;
    uint16_t *avail = (uint16_t *) (ram + ADDR_CTRL_AVAIL);
    avail[2 + (*ctrl_idx % TEST_VSND_VQ_LEN)] = 0;
    (*ctrl_idx)++;
    avail[1] = *ctrl_idx;

    wreg(vsnd, VIRTIO_QueueNotify, TEST_VSND_QUEUE_CTRL);
    assert(*(uint32_t *) (ram + ADDR_CTRL_RESP) == TEST_VSND_S_OK);
    wreg(vsnd, VIRTIO_InterruptACK,
         VIRTIO_INT__USED_RING | VIRTIO_INT__CONF_CHANGE);
    assert(rreg(vsnd, VIRTIO_InterruptStatus) == 0);
}

static void send_pcm_stream_cmd(virtio_snd_state_t *vsnd,
                                uint8_t *ram,
                                uint16_t *ctrl_idx,
                                uint32_t code)
{
    struct {
        uint32_t code;
        uint32_t stream_id;
    } *req = (void *) (ram + ADDR_CTRL_REQ);

    req->code = code;
    req->stream_id = 0;
    send_ctrl_cmd(vsnd, ram, ctrl_idx, sizeof(*req));
}

static void submit_tx(virtio_snd_state_t *vsnd,
                      uint8_t *ram,
                      uint16_t *tx_idx,
                      uint16_t flags)
{
    uint16_t *avail = (uint16_t *) (ram + ADDR_TX_AVAIL);
    avail[0] = flags;
    avail[2 + (*tx_idx % TEST_VSND_VQ_LEN)] = 0;
    (*tx_idx)++;
    avail[1] = *tx_idx;
    wreg(vsnd, VIRTIO_QueueNotify, TEST_VSND_QUEUE_TX);
}

static void wait_used_idx(const uint8_t *ram, uint32_t t_used, uint16_t target)
{
    const uint32_t *used_hdr = (const uint32_t *) (ram + t_used);
    for (int i = 0; i < 400; i++) {
        if ((__atomic_load_n(used_hdr, __ATOMIC_ACQUIRE) >> 16) == target)
            return;
        usleep(5000);
    }
    assert(!"timed out waiting for TX used ring update");
}

static void wait_irq_bit(virtio_snd_state_t *vsnd, uint32_t bit)
{
    for (int i = 0; i < 400; i++) {
        if (rreg(vsnd, VIRTIO_InterruptStatus) & bit)
            return;
        usleep(5000);
    }
    assert(!"timed out waiting for InterruptStatus bit");
}

int main(void)
{
    uint8_t *ram = calloc(1, TEST_RAM_BYTES);
    assert(ram);

    virtio_snd_state_t vsnd = {0};
    vsnd.ram = (uint32_t *) ram;
    /* PortAudio initialization may return false on headless CI runners without
     * a running audio daemon, after the TX worker thread has already started.
     */
    if (!virtio_snd_init(&vsnd))
        assert(vsnd.priv != NULL);

    int pfd[2];
    int rc = pipe(pfd);
    assert(rc == 0);
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(pfd[i], F_GETFL, 0);
        assert(flags >= 0);
        rc = fcntl(pfd[i], F_SETFL, flags | O_NONBLOCK);
        assert(rc == 0);
    }
    virtio_snd_set_wake_fd(pfd[1]);

    wreg(&vsnd, VIRTIO_Status, VIRTIO_STATUS__DRIVER_OK);
    setup_vq(&vsnd, TEST_VSND_QUEUE_CTRL, TEST_VSND_VQ_LEN, ADDR_CTRL_DESC,
             ADDR_CTRL_AVAIL, ADDR_CTRL_USED);
    setup_vq(&vsnd, TEST_VSND_QUEUE_TX, TEST_VSND_VQ_LEN, ADDR_TX_DESC,
             ADDR_TX_AVAIL, ADDR_TX_USED);

    /* Configure stream 0 parameters once before exercising TX wakeups. */
    struct {
        uint32_t code;
        uint32_t stream_id;
        uint32_t buffer_bytes;
        uint32_t period_bytes;
        uint32_t features;
        uint8_t channels;
        uint8_t format;
        uint8_t rate;
        uint8_t padding;
    } *setp = (void *) (ram + ADDR_CTRL_REQ);
    setp->code = TEST_VSND_R_PCM_SET_PARAMS;
    setp->stream_id = 0;
    setp->buffer_bytes = 4096;
    setp->period_bytes = 1024;
    setp->channels = 1;
    setp->format = TEST_VSND_FMT_S16;
    setp->rate = TEST_VSND_RATE_48000;

    uint16_t ctrl_idx = 0;
    send_ctrl_cmd(&vsnd, ram, &ctrl_idx, sizeof(*setp));

    *(uint32_t *) (ram + ADDR_TX_XFER) = 0; /* stream_id = 0 */
    struct virtq_desc *td = (struct virtq_desc *) (ram + ADDR_TX_DESC);
    td[0] = (struct virtq_desc) {ADDR_TX_XFER, 4, VIRTIO_DESC_F_NEXT, 1};
    td[1] = (struct virtq_desc) {ADDR_TX_PCM, 64, VIRTIO_DESC_F_NEXT, 2};
    td[2] = (struct virtq_desc) {ADDR_TX_STATUS, 8, VIRTIO_DESC_F_WRITE, 0};

    /* Run each TX case in its own PCM_PREPARE -> 1 TX -> PCM_RELEASE cycle so
     * tests never block on PortAudio callbacks in audio-less CI environments.
     */
    uint16_t tx_idx = 0;
    struct pollfd poll_read = {pfd[0], POLLIN, 0};
    char buf[16];

    /* 1. TX completion with IRQ enabled writes wake_fd and unblocks poll(). */
    send_pcm_stream_cmd(&vsnd, ram, &ctrl_idx, TEST_VSND_R_PCM_PREPARE);
    submit_tx(&vsnd, ram, &tx_idx, 0);
    int nev = poll(&poll_read, 1, 2000);
    assert(nev == 1 && (poll_read.revents & POLLIN));
    wait_used_idx(ram, ADDR_TX_USED, tx_idx);
    assert(rreg(&vsnd, VIRTIO_InterruptStatus) & VIRTIO_INT__USED_RING);
    ssize_t nread = read(pfd[0], buf, sizeof(buf));
    assert(nread == 1);
    wreg(&vsnd, VIRTIO_InterruptACK, VIRTIO_INT__USED_RING);
    send_pcm_stream_cmd(&vsnd, ram, &ctrl_idx, TEST_VSND_R_PCM_RELEASE);

    /* 2. Suppressed IRQ (VRING_AVAIL_F_NO_INTERRUPT) does not write wake_fd. */
    send_pcm_stream_cmd(&vsnd, ram, &ctrl_idx, TEST_VSND_R_PCM_PREPARE);
    submit_tx(&vsnd, ram, &tx_idx, TEST_VRING_AVAIL_F_NO_INTERRUPT);
    wait_used_idx(ram, ADDR_TX_USED, tx_idx);
    poll_read.revents = 0;
    nev = poll(&poll_read, 1, 50);
    assert(nev == 0);
    assert(rreg(&vsnd, VIRTIO_InterruptStatus) == 0);
    send_pcm_stream_cmd(&vsnd, ram, &ctrl_idx, TEST_VSND_R_PCM_RELEASE);

    /* 3. Clearing wake_fd (-1) stops writes before pipe close. */
    virtio_snd_set_wake_fd(-1);
    send_pcm_stream_cmd(&vsnd, ram, &ctrl_idx, TEST_VSND_R_PCM_PREPARE);
    submit_tx(&vsnd, ram, &tx_idx, 0);
    wait_irq_bit(&vsnd, VIRTIO_INT__USED_RING);
    poll_read.revents = 0;
    nev = poll(&poll_read, 1, 50);
    assert(nev == 0);
    wreg(&vsnd, VIRTIO_InterruptACK, VIRTIO_INT__USED_RING);
    send_pcm_stream_cmd(&vsnd, ram, &ctrl_idx, TEST_VSND_R_PCM_RELEASE);

    /* 4. Error IRQ (virtio_snd_set_fail) from TX worker also writes wake_fd. */
    virtio_snd_set_wake_fd(pfd[1]);
    uint16_t *t_avail = (uint16_t *) (ram + ADDR_TX_AVAIL);
    t_avail[0] = 0;
    t_avail[1] = (uint16_t) (tx_idx + TEST_VSND_VQ_LEN + 1);
    wreg(&vsnd, VIRTIO_QueueNotify, TEST_VSND_QUEUE_TX);
    poll_read.revents = 0;
    nev = poll(&poll_read, 1, 2000);
    assert(nev == 1 && (poll_read.revents & POLLIN));
    assert(rreg(&vsnd, VIRTIO_Status) & VIRTIO_STATUS__DEVICE_NEEDS_RESET);
    assert(rreg(&vsnd, VIRTIO_InterruptStatus) & VIRTIO_INT__CONF_CHANGE);
    nread = read(pfd[0], buf, sizeof(buf));
    assert(nread == 1);

    virtio_snd_set_wake_fd(-1);
    close(pfd[0]);
    close(pfd[1]);
    free(ram);
    printf("PASS: test_vsnd_wake\n");
    return 0;
}
