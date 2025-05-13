#pragma once

#if SEMU_HAS(VIRTIOGPU)

#include "utils.h"

typedef struct {
    struct list_head fence_queue;
} virgl_data_t;

void semu_virgl_init(virtio_gpu_state_t *vgpu);
void semu_virgl_fence_poll(void);
uint32_t semu_virgl_get_num_capsets(void);
#endif
