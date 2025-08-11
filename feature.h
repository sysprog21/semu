#pragma once

/* enable/disable (compile time) features in this header */

/* virtio-blk */
#ifndef SEMU_FEATURE_VIRTIOBLK
#define SEMU_FEATURE_VIRTIOBLK 1
#endif

/* virtio-net */
#ifndef SEMU_FEATURE_VIRTIONET
#define SEMU_FEATURE_VIRTIONET 1
#endif

/* virtio-snd */
#ifndef SEMU_FEATURE_VIRTIOSND
#define SEMU_FEATURE_VIRTIOSND 1
#endif

/* virtio-gpu */
#ifndef SEMU_FEATURE_VIRTIOGPU
#define SEMU_FEATURE_VIRTIOGPU 1
#endif

/* VirGL */
#ifndef SEMU_FEATURE_VIRGL
#define SEMU_FEATURE_VIRGL 1
#endif

/* virtio-input */
#ifndef SEMU_FEATURE_VIRTIOINPUT
#define SEMU_FEATURE_VIRTIOINPUT 1
#endif

/* Feature test macro */
#define SEMU_HAS(x) SEMU_FEATURE_##x
