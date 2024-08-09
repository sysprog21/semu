#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CNFA_IMPLEMENTATION
#include "CNFA_sf.h"

#include "device.h"
#include "riscv.h"
#include "riscv_private.h"
#include "utils.h"
#include "virtio.h"

#define VSND_DEV_CNT_MAX 1

#define VSND_QUEUE_NUM_MAX 1024
#define vsndq (vsnd->queues[vsnd->QueueSel])

#define PRIV(x) ((virtio_snd_config_t *) x->priv)

#define VSND_CNFA_FRAME_SZ 2

enum {
    VSND_QUEUE_CTRL = 0,
    VSND_QUEUE_EVT = 1,
    VSND_QUEUE_TX = 2,
    VSND_QUEUE_RX = 3,
};

/* supported virtio sound version */
enum {
    VSND_FEATURES_0 = 0,
    VSND_FEATURES_1,
};

/* supported control messages */
enum {
    /* jack control requests types */
    VIRTIO_SND_R_JACK_INFO = 1,

    /* PCM control requests types */
    VIRTIO_SND_R_PCM_INFO = 0x0100,
    VIRTIO_SND_R_PCM_SET_PARAMS,
    VIRTIO_SND_R_PCM_PREPARE,
    VIRTIO_SND_R_PCM_RELEASE,
    VIRTIO_SND_R_PCM_START,
    VIRTIO_SND_R_PCM_STOP,

    /* channel map control requests types */
    VIRTIO_SND_R_CHMAP_INFO = 0x0200,

    /* jack event types */
    VIRTIO_SND_EVT_JACK_CONNECTED = 0x1000,
    VIRTIO_SND_EVT_JACK_DISCONNECTED,

    /* PCM event types */
    VIRTIO_SND_EVT_PCM_PERIOD_ELAPSED = 0x1100,
    VIRTIO_SND_EVT_PCM_XRUN,

    /* common status codes */
    VIRTIO_SND_S_OK = 0x8000,
    VIRTIO_SND_S_BAD_MSG,
    VIRTIO_SND_S_NOT_SUPP,
    VIRTIO_SND_S_IO_ERR,
};

/* Unit: Hz */
#define SND_PCM_RATE \
    _(5512)          \
    _(8000)          \
    _(11025)         \
    _(16000)         \
    _(22050)         \
    _(32000)         \
    _(44100)         \
    _(48000)         \
    _(64000)         \
    _(88200)         \
    _(96000)         \
    _(176400)        \
    _(192000)        \
    _(384000)

/* supported PCM frame rates */
enum {
#define _(rate) VIRTIO_SND_PCM_RATE_##rate,
    SND_PCM_RATE
#undef _
};

/* supported PCM frames rates mapping */
static int pcm_rate_tbl[] = {
#define _(rate) [VIRTIO_SND_PCM_RATE_##rate] = rate,
    SND_PCM_RATE
#undef _
};

/* supported PCM stream features */
enum {
    VIRTIO_SND_PCM_F_SHMEM_HOST = 0,
    VIRTIO_SND_PCM_F_SHMEM_GUEST,
    VIRTIO_SND_PCM_F_MSG_POLLING,
    VIRTIO_SND_PCM_F_EVT_SHMEM_PERIODS,
    VIRTIO_SND_PCM_F_EVT_XRUNS,
};

/* supported PCM sample formats */
enum {
/* analog formats (width / physical width) */
#define _(samp_fmt) VIRTIO_SND_PCM_FMT_##samp_fmt
    _(IMA_ADPCM) = 0, /*  4 /  4 bits */
    _(MU_LAW),        /*  8 /  8 bits */
    _(A_LAW),         /*  8 /  8 bits */
    _(S8),            /*  8 /  8 bits */
    _(U8),            /*  8 /  8 bits */
    _(S16),           /* 16 / 16 bits */
    _(U16),           /* 16 / 16 bits */
    _(S18_3),         /* 18 / 24 bits */
    _(U18_3),         /* 18 / 24 bits */
    _(S20_3),         /* 20 / 24 bits */
    _(U20_3),         /* 20 / 24 bits */
    _(S24_3),         /* 24 / 24 bits */
    _(U24_3),         /* 24 / 24 bits */
    _(S20),           /* 20 / 32 bits */
    _(U20),           /* 20 / 32 bits */
    _(S24),           /* 24 / 32 bits */
    _(U24),           /* 24 / 32 bits */
    _(S32),           /* 32 / 32 bits */
    _(U32),           /* 32 / 32 bits */
    _(FLOAT),         /* 32 / 32 bits */
    _(FLOAT64),       /* 64 / 64 bits */
    /* digital formats (width / physical width) */
    _(DSD_U8),          /*  8 /  8 bits */
    _(DSD_U16),         /* 16 / 16 bits */
    _(DSD_U32),         /* 32 / 32 bits */
    _(IEC958_SUBFRAME), /* 32 / 32 bits */
#undef _
};

/* standard channel position definition */
enum {
#define _(chmap_pos) VIRTIO_SND_CHMAP_##chmap_pos
    _(NONE) = 0, /* undefined */
    _(NA),       /* silent */
    _(MONO),     /* mono stream */
    _(FL),       /* front left */
    _(FR),       /* front right */
    _(RL),       /* rear left */
    _(RR),       /* rear right */
    _(FC),       /* front center */
    _(LFE),      /* low frequency (LFE) */
    _(SL),       /* side left */
    _(SR),       /* side right */
    _(RC),       /* rear center */
    _(FLC),      /* front left center */
    _(FRC),      /* front right center */
    _(RLC),      /* rear left center */
    _(RRC),      /* rear right center */
    _(FLW),      /* front left wide */
    _(FRW),      /* front right wide */
    _(FLH),      /* front left high */
    _(FCH),      /* front center high */
    _(FRH),      /* front right high */
    _(TC),       /* top center */
    _(TFL),      /* top front left */
    _(TFR),      /* top front right */
    _(TFC),      /* top front center */
    _(TRL),      /* top rear left */
    _(TRR),      /* top rear right */
    _(TRC),      /* top rear center */
    _(TFLC),     /* top front left center */
    _(TFRC),     /* top front right center */
    _(TSL),      /* top side left */
    _(TSR),      /* top side right */
    _(LLFE),     /* left LFE */
    _(RLFE),     /* right LFE */
    _(BC),       /* bottom center */
    _(BLC),      /* bottom left center */
    _(BRC),      /* bottom right center */
#undef _
};

/* audio data flow direction */
enum {
    VIRTIO_SND_D_OUTPUT = 0,
    VIRTIO_SND_D_INPUT,
};

typedef struct {
    uint32_t jacks;
    uint32_t streams;
    uint32_t chmaps;
    uint32_t controls;
} virtio_snd_config_t;

/* VirtIO sound common header */
typedef struct {
    uint32_t code;
} virtio_snd_hdr_t;

typedef struct {
    uint32_t hda_fn_nid;
} virtio_snd_info_t;

typedef struct {
    virtio_snd_hdr_t hdr;
    uint32_t start_id;
    uint32_t count;
    uint32_t size;
} virtio_snd_query_info_t;

typedef struct {
    virtio_snd_info_t hdr;
    uint32_t features;
    uint32_t hda_reg_defconf;
    uint32_t hda_reg_caps;
    uint8_t connected;
    uint8_t padding[7];
} virtio_snd_jack_info_t;

typedef struct {
    virtio_snd_info_t hdr;
    uint32_t features;
    uint64_t formats;
    uint64_t rates;
    uint8_t direction;
    uint8_t channels_min;
    uint8_t channels_max;
    uint8_t padding[5];
} virtio_snd_pcm_info_t;

typedef struct {
    virtio_snd_hdr_t hdr;
    uint32_t stream_id;
} virtio_snd_pcm_hdr_t;

typedef struct {
    virtio_snd_pcm_hdr_t hdr; /* .code = VIRTIO_SND_R_PCM_SET_PARAMS */
    uint32_t buffer_bytes;
    uint32_t period_bytes;
    uint32_t features; /* 1 << VIRTIO_SND_PCM_F_XXX */
    uint8_t channels;
    uint8_t format;
    uint8_t rate;
    uint8_t padding;
} virtio_snd_pcm_set_params_t;

/* PCM I/O message header */
typedef struct {
    uint32_t stream_id;
} virtio_snd_pcm_xfer_t;

/* PCM I/O message status */
typedef struct {
    uint32_t status;
    uint32_t latency_bytes;
} virtio_snd_pcm_status_t;

#define VIRTIO_SND_CHMAP_MAX_SIZE 18

typedef struct {
    virtio_snd_info_t hdr;
    uint8_t direction;
    uint8_t channels;
    uint8_t positions[VIRTIO_SND_CHMAP_MAX_SIZE];
} virtio_snd_chmap_info_t;

typedef struct {
    pthread_cond_t readable, writable;
    int buf_ev_notity;
    pthread_mutex_t lock;
} virtio_snd_queue_lock_t;

typedef struct {
    int32_t guest_playing;
    uint32_t stream_id;
    int8_t is_done;
    pthread_mutex_t ctrl_mutex;
    pthread_cond_t ctrl_cond;
} vsnd_stream_sel_t;

typedef struct {
    void *addr; /* points to the guest OS buffer */
    uint32_t len;
    uint32_t pos; /* current position in this node */
    struct list_head q;
} vsnd_buf_queue_node_t;

/* hold the settings of each stream */
typedef struct {
    virtio_snd_jack_info_t j;
    virtio_snd_pcm_info_t p;
    virtio_snd_chmap_info_t c;
    virtio_snd_pcm_set_params_t pp;
    struct CNFADriver *audio_host;

    // PCM frame queue lock
    virtio_snd_queue_lock_t lock;
    // PCM frame doubly-ended queue
    vsnd_buf_queue_node_t buf;
    struct list_head buf_queue_head;
    // PCM frame intermediate buffer;
    void *intermediate;

    // playback control
    vsnd_stream_sel_t v;
} virtio_snd_prop_t;

static virtio_snd_config_t vsnd_configs[VSND_DEV_CNT_MAX];
static virtio_snd_prop_t vsnd_props[VSND_DEV_CNT_MAX] = {
    [0 ... VSND_DEV_CNT_MAX - 1].pp.hdr.hdr.code = VIRTIO_SND_R_PCM_SET_PARAMS,
};
static int vsnd_dev_cnt = 0;

static pthread_mutex_t virtio_snd_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t virtio_snd_tx_cond = PTHREAD_COND_INITIALIZER;
static int tx_ev_notify;

/* Forward declaration */
static void virtio_snd_cb(struct CNFADriver *dev,
                          short *out,
                          short *in,
                          int framesp,
                          int framesr);


static void virtio_snd_set_fail(virtio_snd_state_t *vsnd)
{
    vsnd->Status |= VIRTIO_STATUS__DEVICE_NEEDS_RESET;
    if (vsnd->Status & VIRTIO_STATUS__DRIVER_OK)
        vsnd->InterruptStatus |= VIRTIO_INT__CONF_CHANGE;
}

/* Check whether the address is valid or not */
static inline uint32_t vsnd_preprocess(virtio_snd_state_t *vsnd, uint32_t addr)
{
    if ((addr >= RAM_SIZE) || (addr & 0b11))
        return virtio_snd_set_fail(vsnd), 0;

    /* shift right as we have checked in the above */
    return addr >> 2;
}

static void virtio_snd_update_status(virtio_snd_state_t *vsnd, uint32_t status)
{
    vsnd->Status |= status;
    if (status)
        return;

    /* Reset */
    uint32_t *ram = vsnd->ram;
    void *priv = vsnd->priv;
    uint32_t jacks = PRIV(vsnd)->jacks;
    uint32_t streams = PRIV(vsnd)->streams;
    uint32_t chmaps = PRIV(vsnd)->chmaps;
    uint32_t controls = PRIV(vsnd)->controls;
    memset(vsnd, 0, sizeof(*vsnd));
    vsnd->ram = ram;
    vsnd->priv = priv;
    PRIV(vsnd)->jacks = jacks;
    PRIV(vsnd)->streams = streams;
    PRIV(vsnd)->chmaps = chmaps;
    PRIV(vsnd)->controls = controls;
}

static void virtio_snd_read_jack_info_handler(
    virtio_snd_jack_info_t *info,
    const virtio_snd_query_info_t *query,
    uint32_t *plen)
{
    uint32_t cnt = query->count;
    for (uint32_t i = 0; i < cnt; i++) {
        info[i].hdr.hda_fn_nid = 0;
        info[i].features = 0;
        info[i].hda_reg_defconf = 0;
        info[i].hda_reg_caps = 0;
        info[i].connected = 1;
        memset(&info[i].padding, 0, sizeof(info[i].padding));

        virtio_snd_prop_t *props = &vsnd_props[i];
        props->j.hdr.hda_fn_nid = 0;
        props->j.features = 0;
        props->j.hda_reg_defconf = 0;
        props->j.hda_reg_caps = 0;
        props->j.connected = 1;
        memset(&props->j.padding, 0, sizeof(props->j.padding));
    }

    *plen = cnt * sizeof(*info);
}

static void virtio_snd_read_pcm_info_handler(
    virtio_snd_pcm_info_t *info,
    const virtio_snd_query_info_t *query,
    uint32_t *plen)
{
    uint32_t cnt = query->count;
    for (uint32_t i = 0; i < cnt; i++) {
        info[i].hdr.hda_fn_nid = 0;
        info[i].features = 0;
        info[i].formats = (1 << VIRTIO_SND_PCM_FMT_S16);

        info[i].rates = 0;
#define _(rate) info[i].rates |= (1 << VIRTIO_SND_PCM_RATE_##rate);
        SND_PCM_RATE
#undef _
        info[i].direction = VIRTIO_SND_D_OUTPUT;
        info[i].channels_min = 1;
        info[i].channels_max = 1;
        memset(&info[i].padding, 0, sizeof(info[i].padding));

        virtio_snd_prop_t *props = &vsnd_props[i];
        props->p.hdr.hda_fn_nid = 0;
        props->p.features = 0;
        props->p.formats = (1 << VIRTIO_SND_PCM_FMT_S16);
        props->p.rates = (1 << VIRTIO_SND_PCM_RATE_44100);
        props->p.direction = VIRTIO_SND_D_OUTPUT;
        props->p.channels_min = 1;
        props->p.channels_max = 1;
        memset(&props->p.padding, 0, sizeof(props->p.padding));
    }
    *plen = cnt * sizeof(*info);
}

static void virtio_snd_read_chmap_info_handler(
    virtio_snd_chmap_info_t *info,
    const virtio_snd_query_info_t *query,
    uint32_t *plen)
{
    uint32_t cnt = query->count;
    for (uint32_t i = 0; i < cnt; i++) {
        info[i].hdr.hda_fn_nid = 0;
        info[i].direction = VIRTIO_SND_D_OUTPUT;
        info[i].channels = 1;
        info[i].positions[0] = VIRTIO_SND_CHMAP_MONO;

        virtio_snd_prop_t *props = &vsnd_props[i];
        props->c.hdr.hda_fn_nid = 0;
        props->c.direction = VIRTIO_SND_D_OUTPUT;
        props->c.channels = 1;
        props->c.positions[0] = VIRTIO_SND_CHMAP_MONO;
    }
    *plen = cnt * sizeof(info);
}

static void virtio_snd_read_pcm_set_params(
    const virtio_snd_pcm_set_params_t *query,
    uint32_t *plen)
{
    const virtio_snd_pcm_set_params_t *request = query;
    uint32_t id = request->hdr.stream_id;
    virtio_snd_prop_t *props = &vsnd_props[id];
    uint32_t code = props->pp.hdr.hdr.code;
    if (code != VIRTIO_SND_R_PCM_RELEASE &&
        code != VIRTIO_SND_R_PCM_SET_PARAMS &&
        code != VIRTIO_SND_R_PCM_PREPARE) {
        fprintf(stderr,
                "virtio_snd_pcm_set_params with invalid previous state %#08x\n",
                code);
        return;
    }
    /* FIXME: check the valiability of buffer_bytes, period_bytes, channel_min,
     * and channel_max */
    props->pp.hdr.hdr.code = VIRTIO_SND_R_PCM_SET_PARAMS;
    props->pp.buffer_bytes = request->buffer_bytes;
    props->pp.period_bytes = request->period_bytes;
    props->pp.features = request->features;
    props->pp.channels = request->channels;
    props->pp.format = request->format;
    props->pp.rate = request->rate;
    props->pp.padding = request->padding;

    *plen = 0;
}

static void virtio_snd_read_pcm_prepare(const virtio_snd_pcm_hdr_t *query,
                                        uint32_t *plen)
{
    const virtio_snd_pcm_hdr_t *request = query;
    uint32_t stream_id = request->stream_id;
    virtio_snd_prop_t *props = &vsnd_props[stream_id];
    uint32_t code = props->pp.hdr.hdr.code;
    if (code != VIRTIO_SND_R_PCM_RELEASE &&
        code != VIRTIO_SND_R_PCM_SET_PARAMS &&
        code != VIRTIO_SND_R_PCM_PREPARE) {
        fprintf(
            stderr,
            "virtio_snd_read_pcm_prepare with invalid previous state %#08x\n",
            code);
        return;
    }

    props->pp.hdr.hdr.code = VIRTIO_SND_R_PCM_PREPARE;
    props->v.guest_playing = 0;
    props->v.stream_id = stream_id;
    props->v.is_done = 0;

    uint32_t channels = props->pp.channels;
    uint32_t rate = pcm_rate_tbl[props->pp.rate];

    /* Rather use the period_bytes/buffer_bytes set by driver,
     * we calculate bps rate to achieve the strema's desired parameters
     * suggested by [1].
     */
    /* Reference:
     * [1]
     * https://github.com/rust-vmm/vhost-device/blob/main/vhost-device-sound/src/audio_backends/alsa.rs#L153
     */
    /* Calculate bps rate. */
    uint32_t bps_rate = channels * VSND_CNFA_FRAME_SZ * rate;
    /* Calculate period bytes for ~100ms interrupt period. */
    uint32_t cnfa_period_bytes = bps_rate / 10;
    /* Calculate the period size (in frames) for CNFA . */
    uint32_t cnfa_period_frames = cnfa_period_bytes / VSND_CNFA_FRAME_SZ;

    /* The buffer size in frames when calling CNFAInit()
     * is actually period size (i.e., period size then divide
     * the length of frame). */
    /* CNFA only accept frame with signed 16-bit data in little-endian. */
    props->audio_host =
        CNFAInit("ALSA", "semu-virtio-snd", virtio_snd_cb, rate, 0, channels, 0,
                 cnfa_period_frames, NULL, NULL, &props->v);
    pthread_mutex_init(&props->lock.lock, NULL);
    pthread_cond_init(&props->lock.readable, NULL);
    pthread_cond_init(&props->lock.writable, NULL);
    pthread_mutex_init(&props->v.ctrl_mutex, NULL);
    pthread_cond_init(&props->v.ctrl_cond, NULL);
    INIT_LIST_HEAD(&props->buf_queue_head);
    props->intermediate =
        (void *) malloc(sizeof(*props->intermediate) * cnfa_period_bytes);

    *plen = 0;
}

static void virtio_snd_read_pcm_start(const virtio_snd_pcm_hdr_t *query,
                                      uint32_t *plen)
{
    const virtio_snd_pcm_hdr_t *request = query;
    uint32_t stream_id = request->stream_id;
    uint32_t code = vsnd_props[stream_id].pp.hdr.hdr.code;
    if (code != VIRTIO_SND_R_PCM_PREPARE && code != VIRTIO_SND_R_PCM_STOP) {
        fprintf(
            stderr,
            "virtio_snd_read_pcm_start with previous invalide state %#08x\n",
            code);
        return;
    }

    virtio_snd_prop_t *props = &vsnd_props[stream_id];

    /* Control the callback to start playing */
    props->pp.hdr.hdr.code = VIRTIO_SND_R_PCM_START;
    props->v.guest_playing++;
    pthread_cond_signal(&props->v.ctrl_cond);

    *plen = 0;
}

static void virtio_snd_read_pcm_stop(const virtio_snd_pcm_hdr_t *query,
                                     uint32_t *plen)
{
    const virtio_snd_pcm_hdr_t *request = query;
    uint32_t stream_id = request->stream_id;
    uint32_t code = vsnd_props[stream_id].pp.hdr.hdr.code;
    if (code != VIRTIO_SND_R_PCM_START) {
        fprintf(stderr,
                "virtio_snd_read_pcm_stop with previous invalide state %#08x\n",
                code);
        return;
    }

    virtio_snd_prop_t *props = &vsnd_props[stream_id];

    /* Control the callback to stop playing */
    props->pp.hdr.hdr.code = VIRTIO_SND_R_PCM_STOP;
    props->v.guest_playing--;
    pthread_cond_signal(&props->v.ctrl_cond);

    *plen = 0;
}

static void virtio_snd_read_pcm_release(const virtio_snd_pcm_hdr_t *query,
                                        uint32_t *plen)
{
    const virtio_snd_pcm_hdr_t *request = query;
    uint32_t stream_id = request->stream_id;
    virtio_snd_prop_t *props = &vsnd_props[stream_id];
    uint32_t code = props->pp.hdr.hdr.code;
    if (code != VIRTIO_SND_R_PCM_PREPARE && code != VIRTIO_SND_R_PCM_STOP) {
        fprintf(
            stderr,
            "virtio_snd_read_pcm_release with previous invalide state %#08x\n",
            code);
        return;
    }

    props->pp.hdr.hdr.code = VIRTIO_SND_R_PCM_RELEASE;

    /* Tear down PCM buffer related locking attributes. */
    free(props->intermediate);
    /* Explicitly unlock the CVs and mutex. */
    pthread_cond_broadcast(&props->lock.readable);
    pthread_cond_broadcast(&props->lock.writable);
    pthread_mutex_unlock(&props->lock.lock);
    pthread_mutex_destroy(&props->lock.lock);
    pthread_cond_destroy(&props->lock.readable);
    pthread_cond_destroy(&props->lock.writable);

    /* Tear down PCM buffer queue. */
    vsnd_buf_queue_node_t *tmp = NULL;
    vsnd_buf_queue_node_t *node;
    if (!list_empty(&props->buf_queue_head)) {
        list_for_each_entry_safe (node, tmp, &props->buf_queue_head, q) {
            list_del(&node->q);
            free(node);
        }
    }

    props->v.is_done = 1;
    CNFAClose(props->audio_host);

    /* Tear down stream related locking attributes. */
    pthread_cond_broadcast(&props->v.ctrl_cond);
    pthread_mutex_unlock(&props->v.ctrl_mutex);
    pthread_mutex_destroy(&props->v.ctrl_mutex);
    pthread_cond_destroy(&props->v.ctrl_cond);

    *plen = 0;
}

static void __virtio_snd_frame_dequeue(short *out,
                                       uint32_t n,
                                       uint32_t stream_id)
{
    virtio_snd_prop_t *props = &vsnd_props[stream_id];

    pthread_mutex_lock(&props->lock.lock);
    while (props->lock.buf_ev_notity < 1)
        pthread_cond_wait(&props->lock.readable, &props->lock.lock);

    /* Get the PCM frames from queue */
    uint32_t written_bytes = 0;
    memset(props->intermediate, 0, sizeof(*props->intermediate) * n);
    while (!list_empty(&props->buf_queue_head) && written_bytes < n) {
        vsnd_buf_queue_node_t *node =
            list_first_entry(&props->buf_queue_head, vsnd_buf_queue_node_t, q);
        uint32_t left = n - written_bytes;
        uint32_t actual = node->len - node->pos;
        uint32_t len =
            left < actual ? left : actual; /* Naive min implementation */

        memcpy(props->intermediate + written_bytes, node->addr + node->pos,
               len);

        written_bytes += len;
        node->pos += len;
        if (node->pos >= node->len)
            list_del(&node->q);
    }
    memcpy(out, props->intermediate, written_bytes);

    props->lock.buf_ev_notity--;
    pthread_cond_signal(&props->lock.writable);
    pthread_mutex_unlock(&props->lock.lock);
}
static void virtio_snd_cb(struct CNFADriver *dev,
                          short *out,
                          short *in,
                          int framesp,
                          int framesr)
{
    vsnd_stream_sel_t *v_ptr = (vsnd_stream_sel_t *) dev->opaque;
    int channels = dev->channelsPlay;
    uint32_t out_buf_sz = framesp * channels;

    pthread_mutex_lock(&v_ptr->ctrl_mutex);
    if (v_ptr->is_done == 1) {
        memset(out, 0, sizeof(*out) * out_buf_sz);
        goto finally;
    }

    while (v_ptr->guest_playing == 0) {
        memset(out, 0, sizeof(*out) * out_buf_sz);
        pthread_cond_wait(&v_ptr->ctrl_cond, &v_ptr->ctrl_mutex);
    }

    uint32_t id = v_ptr->stream_id;
    uint32_t out_buf_bytes = out_buf_sz * VSND_CNFA_FRAME_SZ;

    __virtio_snd_frame_dequeue(out, out_buf_bytes, id);

finally:
    pthread_mutex_unlock(&v_ptr->ctrl_mutex);
}

#define VSND_DESC_CNT 3
static int virtio_snd_desc_handler(virtio_snd_state_t *vsnd,
                                   const virtio_snd_queue_t *queue,
                                   uint32_t desc_idx,
                                   uint32_t *plen)
{
    /* A control message uses at most 3 virtqueue descriptors, where
     * the first descriptor contains:
     *   struct virtio_snd_hdr hdr
     * the second descriptors contains:
     *   struct virtio_snd_hdr hdr
     * if needed, the third descriptors contains the response payload structure.
     */
    struct virtq_desc vq_desc[VSND_DESC_CNT];

    /* Collect the descriptors */
    for (int i = 0; i < VSND_DESC_CNT; i++) {
        /* The size of the `struct virtq_desc` is 4 words */
        const uint32_t *desc = &vsnd->ram[queue->QueueDesc + desc_idx * 4];

        /* Retrieve the fields of current descriptor */
        vq_desc[i].addr = desc[0];
        vq_desc[i].len = desc[2];
        vq_desc[i].flags = desc[3];
        desc_idx = desc[3] >> 16; /* vq_desc[desc_cnt].next */

        /* Leave the loop if next-flag is not set */
        if (!(vq_desc[i].flags & VIRTIO_DESC_F_NEXT))
            break;
    }

    /* Process the header */
    const virtio_snd_hdr_t *request =
        (virtio_snd_hdr_t *) ((uintptr_t) vsnd->ram + vq_desc[0].addr);
    uint32_t type = request->code;
    virtio_snd_hdr_t *response =
        (virtio_snd_hdr_t *) ((uintptr_t) vsnd->ram + vq_desc[1].addr);
    const void *query = (void *) ((uintptr_t) vsnd->ram + vq_desc[0].addr);

    /* As there are plenty of structures for response payload,
     * use a void pointer for generic type support.
     */
    void *info = (void *) (uintptr_t) vsnd->ram + vq_desc[2].addr;

    /* Process the data */
    switch (type) {
    case VIRTIO_SND_R_JACK_INFO:
        virtio_snd_read_jack_info_handler(info, query, plen);
        break;
    case VIRTIO_SND_R_PCM_INFO:
        virtio_snd_read_pcm_info_handler(info, query, plen);
        break;
    case VIRTIO_SND_R_CHMAP_INFO:
        virtio_snd_read_chmap_info_handler(info, query, plen);
        break;
    case VIRTIO_SND_R_PCM_SET_PARAMS:
        virtio_snd_read_pcm_set_params(query, plen);
        break;
    case VIRTIO_SND_R_PCM_PREPARE:
        virtio_snd_read_pcm_prepare(query, plen);
        break;
    case VIRTIO_SND_R_PCM_RELEASE:
        virtio_snd_read_pcm_release(query, plen);
        break;
    case VIRTIO_SND_R_PCM_START:
        virtio_snd_read_pcm_start(query, plen);
        break;
    case VIRTIO_SND_R_PCM_STOP:
        virtio_snd_read_pcm_stop(query, plen);
        break;
    default:
        fprintf(stderr, "%d: unsupported virtio-snd operation!\n", type);
        response->code = VIRTIO_SND_S_NOT_SUPP;
        *plen = 0;
        return -1;
    }

    /* Return the device status */
    response->code = VIRTIO_SND_S_OK;

    return 0;
}


static void __virtio_snd_frame_enqueue(void *payload,
                                       uint32_t n,
                                       uint32_t stream_id)
{
    virtio_snd_prop_t *props = &vsnd_props[stream_id];

    pthread_mutex_lock(&props->lock.lock);
    while (props->lock.buf_ev_notity > 0)
        pthread_cond_wait(&props->lock.writable, &props->lock.lock);

    /* Add a PCM frame to queue */
    /* As stated in Linux Kernel mailing list [1], we keep the pointer
     * points to the payload [2] so that we can always get up-to-date
     * contents of PCM frames.
     * References:
     * [1] https://lore.kernel.org/all/ZQHPeD0fds9sYzHO@pc-79.home/T/
     * [2]
     * https://github.com/rust-vmm/vhost-device/blob/eb2e2227e41d48a52e4e6346189b772c5363879d/staging/vhost-device-sound/src/device.rs#L554
     */
    /* FIXME: locate the root case of repeating artifact even we
     * keep the pointer of the payload.
     */
    vsnd_buf_queue_node_t *node = malloc(sizeof(*node));
    node->addr = payload;
    node->len = n;
    node->pos = 0;
    list_push(&node->q, &props->buf_queue_head);

    pthread_mutex_unlock(&props->lock.lock);
}

typedef struct {
    struct virtq_desc vq_desc;
    struct list_head q;
} virtq_desc_queue_node_t;
static int virtio_snd_tx_desc_handler(virtio_snd_state_t *vsnd,
                                      const virtio_snd_queue_t *queue,
                                      uint32_t desc_idx,
                                      uint32_t *plen)
{
    /* A PCM I/O message uses at least 3 virtqueue descriptors to
     * represent a PCM data of a period size.
     * The first part contains one descriptor as follows:
     *   struct virtio_snd_pcm_xfer
     * The second part contains one or more descriptors
     * representing PCM frames.
     * the last part contains one descriptor as follows:
     *   struct virtio_snd_pcm_status
     */
    virtq_desc_queue_node_t *node;
    struct list_head q;
    INIT_LIST_HEAD(&q);

    /* Collect the descriptors */
    int cnt = 0;
    for (;;) {
        /* The size of the `struct virtq_desc` is 4 words */
        const uint32_t *desc = &vsnd->ram[queue->QueueDesc + desc_idx * 4];

        /* Retrieve the fields of current descriptor */
        node = (virtq_desc_queue_node_t *) malloc(sizeof(*node));
        node->vq_desc.addr = desc[0];
        node->vq_desc.len = desc[2];
        node->vq_desc.flags = desc[3];
        list_push(&node->q, &q);
        desc_idx = desc[3] >> 16; /* vq_desc[desc_cnt].next */

        cnt++;

        /* Leave the loop if next-flag is not set */
        if (!(desc[3] & VIRTIO_DESC_F_NEXT))
            break;
    }

    int idx = 0;
    uint32_t stream_id = 0; /* Explicitly set the stream_id */
    uintptr_t base = (uintptr_t) vsnd->ram;
    uint32_t ret_len = 0;
    list_for_each_entry (node, &q, q) {
        uint32_t addr = node->vq_desc.addr;
        uint32_t len = node->vq_desc.len;
        if (idx == 0) { /* the first descriptor */
            const virtio_snd_pcm_xfer_t *request =
                (virtio_snd_pcm_xfer_t *) (base + addr);
            stream_id = request->stream_id;
            goto early_continue;
        } else if (idx == cnt - 1) { /* the last descriptor */
            virtio_snd_pcm_status_t *response =
                (virtio_snd_pcm_status_t *) (base + addr);
            response->status = VIRTIO_SND_S_OK;
            response->latency_bytes = ret_len;
            *plen = sizeof(*response);
            goto early_continue;
        }

        void *payload = (void *) (base + addr);
        __virtio_snd_frame_enqueue(payload, len, stream_id);
        ret_len += len;

    early_continue:
        idx++;
    }

    virtio_snd_prop_t *props = &vsnd_props[stream_id];
    props->lock.buf_ev_notity++;
    pthread_cond_signal(&props->lock.readable);

    /* Tear down the descriptor list and free space. */
    virtq_desc_queue_node_t *tmp = NULL;
    list_for_each_entry_safe (node, tmp, &q, q) {
        list_del(&node->q);
        free(node);
    }

    return 0;
}

static void virtio_queue_notify_handler(
    virtio_snd_state_t *vsnd,
    int index,
    int (*handler)(virtio_snd_state_t *,
                   const virtio_snd_queue_t *,
                   uint32_t,
                   uint32_t *))
{
    uint32_t *ram = vsnd->ram;
    virtio_snd_queue_t *queue = &vsnd->queues[index];
    if (vsnd->Status & VIRTIO_STATUS__DEVICE_NEEDS_RESET)
        return;

    if (!((vsnd->Status & VIRTIO_STATUS__DRIVER_OK) && queue->ready))
        return virtio_snd_set_fail(vsnd);

    /* Check for new buffers */
    uint16_t new_avail = ram[queue->QueueAvail] >> 16;
    if (new_avail - queue->last_avail > (uint16_t) queue->QueueNum)
        return virtio_snd_set_fail(vsnd);

    if (queue->last_avail == new_avail)
        return;

    /* Process them */
    uint16_t new_used = ram[queue->QueueUsed] >> 16; /* virtq_used.idx (le16) */
    while (queue->last_avail != new_avail) {
        /* Obtain the index in the ring buffer */
        uint16_t queue_idx = queue->last_avail % queue->QueueNum;

        /* Since each buffer index occupies 2 bytes but the memory is aligned
         * with 4 bytes, and the first element of the available queue is stored
         * at ram[queue->QueueAvail + 1], to acquire the buffer index, it
         * requires the following array index calculation and bit shifting.
         * Check also the `struct virtq_avail` on the spec.
         */
        uint16_t buffer_idx = ram[queue->QueueAvail + 1 + queue_idx / 2] >>
                              (16 * (queue_idx % 2));

        /* Consume request from the available queue and process the data in the
         * descriptor list.
         */
        uint32_t len = 0;
        int result = handler(vsnd, queue, buffer_idx, &len);
        if (result != 0)
            return virtio_snd_set_fail(vsnd);

        /* Write used element information (`struct virtq_used_elem`) to the used
         * queue */
        uint32_t vq_used_addr =
            queue->QueueUsed + 1 + (new_used % queue->QueueNum) * 2;
        ram[vq_used_addr] = buffer_idx; /* virtq_used_elem.id  (le32) */
        ram[vq_used_addr + 1] = len;    /* virtq_used_elem.len (le32) */
        queue->last_avail++;
        new_used++;
    }

    /* Check le32 len field of struct virtq_used_elem on the spec  */
    vsnd->ram[queue->QueueUsed] &= MASK(16); /* Reset low 16 bits to zero */
    vsnd->ram[queue->QueueUsed] |= ((uint32_t) new_used) << 16; /* len */

    /* Send interrupt, unless VIRTQ_AVAIL_F_NO_INTERRUPT is set */
    if (!(ram[queue->QueueAvail] & 1))
        vsnd->InterruptStatus |= VIRTIO_INT__USED_RING;
}

/* TX thread context */
/* Receive PCM frames from driver. */
static void *func(void *args)
{
    virtio_snd_state_t *vsnd = (virtio_snd_state_t *) args;
    for (;;) {
        pthread_mutex_lock(&virtio_snd_mutex);
        while (tx_ev_notify <= 0)
            pthread_cond_wait(&virtio_snd_tx_cond, &virtio_snd_mutex);

        tx_ev_notify--;
        virtio_queue_notify_handler(vsnd, 2, virtio_snd_tx_desc_handler);

        pthread_mutex_unlock(&virtio_snd_mutex);
    }
    pthread_exit(NULL);
}

static bool virtio_snd_reg_read(virtio_snd_state_t *vsnd,
                                uint32_t addr,
                                uint32_t *value)
{
#define _(reg) VIRTIO_##reg
    switch (addr) {
    case _(MagicValue):
        *value = 0x74726976;
        return true;
    case _(Version):
        *value = 2;
        return true;
    case _(DeviceID):
        *value = 25;
        return true;
    case _(VendorID):
        *value = VIRTIO_VENDOR_ID;
        return true;
    case _(DeviceFeatures):
        *value = vsnd->DeviceFeaturesSel == 0
                     ? VSND_FEATURES_0
                     : (vsnd->DeviceFeaturesSel == 1 ? VSND_FEATURES_1 : 0);
        return true;
    case _(QueueNumMax):
        *value = VSND_QUEUE_NUM_MAX;
        return true;
    case _(QueueReady):
        *value = vsndq.ready ? 1 : 0;
        return true;
    case _(InterruptStatus):
        *value = vsnd->InterruptStatus;
        return true;
    case _(Status):
        *value = vsnd->Status;
        return true;
    case _(ConfigGeneration):
        *value = 0;
        return true;
    default:
        /* Invalid address which exceeded the range */
        if (!RANGE_CHECK(addr, _(Config), sizeof(virtio_snd_config_t)))
            return false;

        /* Read configuration from the corresponding register */
        *value = ((uint32_t *) PRIV(vsnd))[addr - _(Config)];

        return true;
    }
#undef _
}
static bool virtio_snd_reg_write(virtio_snd_state_t *vsnd,
                                 uint32_t addr,
                                 uint32_t value)
{
#define _(reg) VIRTIO_##reg
    switch (addr) {
    case _(DeviceFeaturesSel):
        vsnd->DeviceFeaturesSel = value;
        return true;
    case _(DriverFeatures):
        vsnd->DriverFeaturesSel == 0 ? (vsnd->DriverFeatures = value) : 0;
        return true;
    case _(DriverFeaturesSel):
        vsnd->DriverFeaturesSel = value;
        return true;
    case _(QueueSel):
        if (value < ARRAY_SIZE(vsnd->queues))
            vsnd->QueueSel = value;
        else
            virtio_snd_set_fail(vsnd);
        return true;
    case _(QueueNum):
        if (value > 0 && value <= VSND_QUEUE_NUM_MAX)
            vsndq.QueueNum = value;
        else
            virtio_snd_set_fail(vsnd);
        return true;
    case _(QueueReady):
        vsndq.ready = value & 1;
        if (value & 1)
            vsndq.last_avail = vsnd->ram[vsndq.QueueAvail] >> 16;
        return true;
    case _(QueueDescLow):
        vsndq.QueueDesc = vsnd_preprocess(vsnd, value);
        return true;
    case _(QueueDescHigh):
        if (value)
            virtio_snd_set_fail(vsnd);
        return true;
    case _(QueueDriverLow):
        vsndq.QueueAvail = vsnd_preprocess(vsnd, value);
        return true;
    case _(QueueDriverHigh):
        if (value)
            virtio_snd_set_fail(vsnd);
        return true;
    case _(QueueDeviceLow):
        vsndq.QueueUsed = vsnd_preprocess(vsnd, value);
        return true;
    case _(QueueDeviceHigh):
        if (value)
            virtio_snd_set_fail(vsnd);
        return true;
    case _(QueueNotify):
        if (value < ARRAY_SIZE(vsnd->queues)) {
            switch (value) {
            case VSND_QUEUE_CTRL:
                virtio_queue_notify_handler(vsnd, value,
                                            virtio_snd_desc_handler);
                break;
            case VSND_QUEUE_TX:
                tx_ev_notify++;
                pthread_cond_signal(&virtio_snd_tx_cond);
                break;
            default:
                fprintf(stderr, "value %d not supported\n", value);
                return false;
            }
        } else {
            virtio_snd_set_fail(vsnd);
        }


        return true;
    case _(InterruptACK):
        vsnd->InterruptStatus &= ~value;
        return true;
    case _(Status):
        virtio_snd_update_status(vsnd, value);
        return true;
    default:
        /* Invalid address which exceeded the range */
        if (!RANGE_CHECK(addr, _(Config), sizeof(virtio_snd_config_t)))
            return false;

        /* Write configuration to the corresponding register */
        ((uint32_t *) PRIV(vsnd))[addr - _(Config)] = value;

        return true;
    }
#undef _
}
void virtio_snd_read(hart_t *vm,
                     virtio_snd_state_t *vsnd,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value)
{
    switch (width) {
    case RV_MEM_LW:
        if (!virtio_snd_reg_read(vsnd, addr >> 2, value))
            vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
        break;
    case RV_MEM_LBU:
    case RV_MEM_LB:
    case RV_MEM_LHU:
    case RV_MEM_LH:
        vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
        break;

    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        break;
    }
}
void virtio_snd_write(hart_t *vm,
                      virtio_snd_state_t *vsnd,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value)
{
    switch (width) {
    case RV_MEM_SW:
        if (!virtio_snd_reg_write(vsnd, addr >> 2, value))
            vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
        break;
    case RV_MEM_SB:
    case RV_MEM_SH:
        vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
        break;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        break;
    }
}

bool virtio_snd_init(virtio_snd_state_t *vsnd)
{
    if (vsnd_dev_cnt >= VSND_DEV_CNT_MAX) {
        fprintf(stderr,
                "Exceeded the number of virtio-snd devices that can be "
                "allocated.\n");
        return false;
    }

    /* Allocate the memory of private member. */
    vsnd->priv = &vsnd_configs[vsnd_dev_cnt++];

    PRIV(vsnd)->jacks = 1;
    PRIV(vsnd)->streams = 1;
    PRIV(vsnd)->chmaps = 1;
    PRIV(vsnd)->controls =
        0; /* virtio-snd device does not support control elements */

    tx_ev_notify = 0;
    pthread_t tid;
    if (pthread_create(&tid, NULL, func, vsnd) != 0) {
        fprintf(stderr, "cannot create TX thread\n");
        return false;
    }

    return true;
}
