#pragma once

/* enable/disable (compile time) features in this header */

/* virtio-blk */
#ifndef SEMU_FEATUREVIRTIOBLK
#define SEMU_FEATUREVIRTIOBLK 1
#endif

/* virtio-net */
#ifndef SEMU_FEATUREVIRTIONET
#define SEMU_FEATUREVIRTIONET 1
#endif

/* Feature test macro */
#define SEMU_HAS(x) SEMU_FEATURE_##x
