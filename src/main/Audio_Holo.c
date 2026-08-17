#include "Core.h"

#if CC_AUD_BACKEND == CC_AUD_BACKEND_HOLO
#include "Audio.h"
#include "Errors.h"
#include "Funcs.h"
#include "Platform.h"
#include "String_.h"
#include "holo_port.h"
#include "Audio_Holo.h"

#define HOLO_AUDIO_RATE         22050
#define HOLO_AUDIO_CHANNELS     1
#define HOLO_AUDIO_BITS         16
#define HOLO_AUDIO_BLOCK_FRAMES 256
#define HOLO_AUDIO_BLOCK_BYTES  (HOLO_AUDIO_BLOCK_FRAMES * sizeof(cc_int16))
#define HOLO_AUDIO_VOICES       8
#define HOLO_AUDIO_STACK_BYTES  (8u * 1024u)

struct AudioContext {
    const cc_int16 *samples;
    cc_uint32 sampleBytes;
    cc_uint32 frameCount;
    cc_uint32 phase;
    cc_uint32 phaseStep;
    int count;
    int channels;
    int sampleRate;
    int volume;
    cc_bool queued;
    cc_bool active;
};

#define POOL_MAX_CONTEXTS HOLO_AUDIO_VOICES
#include "_AudioBase.h"

static struct AudioContext *s_contexts[HOLO_AUDIO_VOICES];
static module_sync_handle_t s_audio_lock;
static module_sync_handle_t s_audio_done;
static void *s_audio_stream;
static void *s_audio_task;
static volatile cc_bool s_audio_stop;
static volatile cc_bool s_audio_worker_running;
static volatile cc_uint32 s_audio_mixed_blocks;
static volatile cc_uint32 s_audio_active_voices;
static volatile int32_t s_audio_last_error;
static cc_int16 s_audio_mix[HOLO_AUDIO_BLOCK_FRAMES];

static const module_host_api_v2 *AudioHost(void) {
    return holo_port_host();
}

static cc_bool AudioLock(void) {
    const module_host_api_v2 *host = AudioHost();
    return host && s_audio_lock && host->sync.take &&
           host->sync.take(s_audio_lock, MODULE_WAIT_FOREVER) == MODULE_OK;
}

static void AudioUnlock(void) {
    const module_host_api_v2 *host = AudioHost();
    if (host && s_audio_lock && host->sync.give) (void)host->sync.give(s_audio_lock);
}

static void AudioFail(int32_t result, const char *message) {
    if (!result) result = MODULE_ERR_IO;
    s_audio_last_error = result;
    holo_port_log(message);
}

static cc_bool AudioHasVoices(void) {
    int i;
    cc_bool active = false;
    if (!AudioLock()) return false;
    for (i = 0; i < HOLO_AUDIO_VOICES; i++) {
        if (s_contexts[i] && s_contexts[i]->active) { active = true; break; }
    }
    AudioUnlock();
    return active;
}

static cc_int16 AudioClampSample(int value) {
    if (value >  32767) return  32767;
    if (value < -32768) return -32768;
    return (cc_int16)value;
}

static void AudioMixBlock(void) {
    int frame, slot;
    cc_uint32 active = 0;

    if (!AudioLock()) {
        Mem_Set(s_audio_mix, 0, sizeof(s_audio_mix));
        return;
    }

    for (frame = 0; frame < HOLO_AUDIO_BLOCK_FRAMES; frame++) {
        int mixed = 0;
        for (slot = 0; slot < HOLO_AUDIO_VOICES; slot++) {
            struct AudioContext *ctx = s_contexts[slot];
            cc_uint32 sourceFrame;
            int sample;
            if (!ctx || !ctx->active) continue;

            sourceFrame = ctx->phase >> 16;
            if (sourceFrame >= ctx->frameCount) {
                ctx->active = false;
                ctx->queued = false;
                continue;
            }

            if (ctx->channels == 2) {
                int left  = ctx->samples[sourceFrame * 2 + 0];
                int right = ctx->samples[sourceFrame * 2 + 1];
                sample = (left + right) / 2;
            } else {
                sample = ctx->samples[sourceFrame];
            }
            mixed += sample * ctx->volume / 100;
            ctx->phase += ctx->phaseStep;
            if ((ctx->phase >> 16) >= ctx->frameCount) {
                ctx->active = false;
                ctx->queued = false;
            }
        }
        s_audio_mix[frame] = AudioClampSample(mixed);
    }

    for (slot = 0; slot < HOLO_AUDIO_VOICES; slot++) {
        if (s_contexts[slot] && s_contexts[slot]->active) active++;
    }
    s_audio_active_voices = active;
    AudioUnlock();
}

static void AudioWorker(void *arg) {
    const module_host_api_v2 *host = AudioHost();
    (void)arg;
    s_audio_worker_running = true;

    while (!s_audio_stop) {
        size_t available = 0;
        size_t written = 0;
        int32_t result;

        if (!AudioHasVoices()) {
            if (host && host->task.delay) host->task.delay(4);
            continue;
        }
        result = host->audio.available(s_audio_stream, &available);
        if (result != MODULE_OK) {
            AudioFail(result, "[cc.audio] ERROR: querying audio output capacity failed");
            break;
        }
        if (available < HOLO_AUDIO_BLOCK_BYTES) {
            if (host->task.delay) host->task.delay(1);
            continue;
        }

        AudioMixBlock();
        result = host->audio.write(s_audio_stream, s_audio_mix,
                                   HOLO_AUDIO_BLOCK_BYTES, &written);
        if (result != MODULE_OK || written != HOLO_AUDIO_BLOCK_BYTES) {
            AudioFail(result ? result : MODULE_ERR_IO,
                      "[cc.audio] ERROR: writing mixed PCM block failed");
            break;
        }
        s_audio_mixed_blocks++;
    }

    s_audio_worker_running = false;
    s_audio_active_voices = 0;
    if (host && host->sync.give && s_audio_done) (void)host->sync.give(s_audio_done);
    if (host && host->task.remove) host->task.remove(NULL);
    for (;;) {
        if (host && host->task.delay) host->task.delay(1000);
    }
}

cc_bool AudioBackend_Init(void) {
    const module_host_api_v2 *host = AudioHost();
    module_audio_desc_t desc;
    void *task = NULL;
    int32_t result;

    if (s_audio_stream && s_audio_task && s_audio_worker_running) return true;
    if (!host || !host->audio.begin || !host->audio.write || !host->audio.available ||
        !host->audio.end || !host->task.create_ex || !host->task.delay ||
        !host->sync.create_mutex || !host->sync.create_counting ||
        !host->sync.take || !host->sync.give || !host->sync.destroy) {
        AudioFail(MODULE_ERR_UNSUPPORTED,
                  "[cc.audio] ERROR: host audio/task/sync API is incomplete");
        return false;
    }

    Mem_Set(s_contexts, 0, sizeof(s_contexts));
    s_audio_stop = false;
    s_audio_worker_running = false;
    s_audio_mixed_blocks = 0;
    s_audio_active_voices = 0;
    s_audio_last_error = MODULE_OK;

    result = host->sync.create_mutex(&s_audio_lock);
    if (result != MODULE_OK) {
        AudioFail(result, "[cc.audio] ERROR: creating mixer mutex failed");
        return false;
    }
    result = host->sync.create_counting(1, 0, &s_audio_done);
    if (result != MODULE_OK) {
        AudioFail(result, "[cc.audio] ERROR: creating mixer completion event failed");
        host->sync.destroy(s_audio_lock); s_audio_lock = NULL;
        return false;
    }

    Mem_Set(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.sample_rate = HOLO_AUDIO_RATE;
    desc.bits_per_sample = HOLO_AUDIO_BITS;
    desc.channels = HOLO_AUDIO_CHANNELS;
    result = host->audio.begin(&desc, &s_audio_stream);
    if (result != MODULE_OK || !s_audio_stream) {
        AudioFail(result ? result : MODULE_ERR_IO,
                  "[cc.audio] ERROR: opening 22050 Hz mono output failed");
        host->sync.destroy(s_audio_done); s_audio_done = NULL;
        host->sync.destroy(s_audio_lock); s_audio_lock = NULL;
        s_audio_stream = NULL;
        return false;
    }

    /* Core 0 is intentional: rendering/gameplay stays on Core 1. The stack is
     * placed in PSRAM, while the host retains internal DMA memory ownership. */
    s_audio_worker_running = true;
    result = host->task.create_ex("cc_audio", AudioWorker, NULL,
                                  HOLO_AUDIO_STACK_BYTES, 4u, 0,
                                  MODULE_HEAP_PSRAM | MODULE_HEAP_8BIT, &task);
    if (result != MODULE_OK || !task) {
        s_audio_worker_running = false;
        AudioFail(result ? result : MODULE_ERR_NO_MEMORY,
                  "[cc.audio] ERROR: creating Core 0 PSRAM audio task failed");
        (void)host->audio.end(s_audio_stream); s_audio_stream = NULL;
        host->sync.destroy(s_audio_done); s_audio_done = NULL;
        host->sync.destroy(s_audio_lock); s_audio_lock = NULL;
        return false;
    }
    s_audio_task = task;
    holo_port_log("[cc.audio] output=22050Hz S16 mono mixer=8 task=Core0 stack=PSRAM");
    return true;
}

void Audio_HoloShutdown(void) {
    const module_host_api_v2 *host = AudioHost();
    if (!host) return;

    s_audio_stop = true;
    if (s_audio_task && s_audio_done && host->sync.take) {
        int32_t stopped = host->sync.take(s_audio_done, 2000);
        if (stopped != MODULE_OK && host->task.remove) {
            holo_port_log("[cc.audio] ERROR: Core 0 audio task stop timed out; forcing removal");
            host->task.remove(s_audio_task);
        }
    }
    s_audio_task = NULL;
    s_audio_worker_running = false;

    if (s_audio_stream && host->audio.end) {
        int32_t result = host->audio.end(s_audio_stream);
        if (result != MODULE_OK && s_audio_last_error == MODULE_OK)
            s_audio_last_error = result;
    }
    s_audio_stream = NULL;

    if (host->sync.destroy) {
        if (s_audio_done) host->sync.destroy(s_audio_done);
        if (s_audio_lock) host->sync.destroy(s_audio_lock);
    }
    s_audio_done = NULL;
    s_audio_lock = NULL;
    s_audio_active_voices = 0;
    Mem_Set(s_contexts, 0, sizeof(s_contexts));
}

void AudioBackend_Tick(void) { }
void AudioBackend_Free(void) { Audio_HoloShutdown(); }

cc_result Audio_Init(struct AudioContext *ctx, int buffers) {
    int i;
    if (!ctx || buffers <= 0 || !s_audio_worker_running) return ERR_NOT_SUPPORTED;
    if (!AudioLock()) return ERR_NOT_SUPPORTED;
    for (i = 0; i < HOLO_AUDIO_VOICES; i++) {
        if (s_contexts[i] == ctx) break;
        if (!s_contexts[i]) { s_contexts[i] = ctx; break; }
    }
    if (i == HOLO_AUDIO_VOICES) {
        AudioUnlock();
        return ERR_OUT_OF_MEMORY;
    }
    Mem_Set(ctx, 0, sizeof(*ctx));
    ctx->count = buffers;
    ctx->volume = 100;
    AudioUnlock();
    return 0;
}

void Audio_Close(struct AudioContext *ctx) {
    int i;
    if (!ctx) return;
    if (AudioLock()) {
        for (i = 0; i < HOLO_AUDIO_VOICES; i++) {
            if (s_contexts[i] == ctx) s_contexts[i] = NULL;
        }
        Mem_Set(ctx, 0, sizeof(*ctx));
        AudioUnlock();
    } else {
        Mem_Set(ctx, 0, sizeof(*ctx));
    }
}

static cc_result Audio_SetFormat(struct AudioContext *ctx, int channels,
                                 int sampleRate, int playbackRate) {
    if (!ctx || (channels != 1 && channels != 2) || sampleRate <= 0 ||
        playbackRate <= 0) return ERR_INVALID_ARGUMENT;
    if (!AudioLock()) return ERR_NOT_SUPPORTED;
    ctx->channels = channels;
    ctx->sampleRate = Audio_AdjustSampleRate(sampleRate, playbackRate);
    AudioUnlock();
    return 0;
}

void Audio_SetVolume(struct AudioContext *ctx, int volume) {
    if (!ctx) return;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    if (!AudioLock()) return;
    ctx->volume = volume;
    AudioUnlock();
}

static cc_result Audio_QueueChunk(struct AudioContext *ctx, struct AudioChunk *chunk) {
    if (!ctx || !chunk || !chunk->data || !chunk->size) return ERR_INVALID_ARGUMENT;
    if (!AudioLock()) return ERR_NOT_SUPPORTED;
    if (ctx->active || ctx->queued) {
        AudioUnlock();
        return ERR_INVALID_ARGUMENT;
    }
    ctx->samples = (const cc_int16 *)chunk->data;
    ctx->sampleBytes = chunk->size;
    ctx->queued = true;
    AudioUnlock();
    return 0;
}

static cc_result Audio_Play(struct AudioContext *ctx) {
    cc_uint64 step;
    if (!ctx || !AudioLock()) return ERR_NOT_SUPPORTED;
    if (!ctx->queued || !ctx->samples || !ctx->channels || !ctx->sampleRate) {
        AudioUnlock();
        return ERR_INVALID_ARGUMENT;
    }
    ctx->frameCount = ctx->sampleBytes / (sizeof(cc_int16) * ctx->channels);
    if (!ctx->frameCount) {
        ctx->queued = false;
        AudioUnlock();
        return ERR_INVALID_ARGUMENT;
    }
    step = ((cc_uint64)(cc_uint32)ctx->sampleRate << 16) / HOLO_AUDIO_RATE;
    ctx->phaseStep = step ? (cc_uint32)step : 1;
    ctx->phase = 0;
    ctx->active = true;
    AudioUnlock();
    return 0;
}

static cc_result Audio_Poll(struct AudioContext *ctx, int *inUse) {
    if (!ctx || !inUse) return ERR_INVALID_ARGUMENT;
    if (s_audio_last_error != MODULE_OK) return (cc_result)s_audio_last_error;
    if (!AudioLock()) return ERR_NOT_SUPPORTED;
    *inUse = ctx->active || ctx->queued ? 1 : 0;
    AudioUnlock();
    return 0;
}

cc_result StreamContext_SetFormat(struct AudioContext *ctx, int channels,
                                  int sampleRate, int playbackRate) {
    return Audio_SetFormat(ctx, channels, sampleRate, playbackRate);
}
cc_result StreamContext_Enqueue(struct AudioContext *ctx, struct AudioChunk *chunk) {
    return Audio_QueueChunk(ctx, chunk);
}
cc_result StreamContext_Play(struct AudioContext *ctx) { return Audio_Play(ctx); }
cc_result StreamContext_Pause(struct AudioContext *ctx) { (void)ctx; return ERR_NOT_SUPPORTED; }
cc_result StreamContext_Update(struct AudioContext *ctx, int *inUse) {
    return Audio_Poll(ctx, inUse);
}

cc_bool SoundContext_FastPlay(struct AudioContext *ctx, struct AudioData *data) {
    (void)ctx; (void)data;
    return true;
}
cc_result SoundContext_PlayData(struct AudioContext *ctx, struct AudioData *data) {
    cc_result result;
    if ((result = Audio_SetFormat(ctx, data->channels, data->sampleRate, data->rate))) return result;
    if ((result = Audio_QueueChunk(ctx, &data->chunk))) return result;
    return Audio_Play(ctx);
}
cc_result SoundContext_PollBusy(struct AudioContext *ctx, cc_bool *isBusy) {
    int inUse = 0;
    cc_result result = Audio_Poll(ctx, &inUse);
    if (!result) *isBusy = inUse != 0;
    return result;
}

cc_bool Audio_DescribeError(cc_result result, cc_string *dst) {
    String_Format1(dst, "Holo audio error %i", &result);
    return true;
}

int Audio_HoloActive(void) {
    return s_audio_stream && s_audio_worker_running && s_audio_last_error == MODULE_OK;
}
uint32_t Audio_HoloMixedBlocks(void) { return s_audio_mixed_blocks; }
uint32_t Audio_HoloActiveVoices(void) { return s_audio_active_voices; }
int32_t Audio_HoloLastError(void) { return s_audio_last_error; }
#endif
