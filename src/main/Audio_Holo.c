#include "Core.h"

#if CC_AUD_BACKEND == CC_AUD_BACKEND_HOLO
#include "Audio.h"
#include "Errors.h"
#include "Funcs.h"
#include "Platform.h"
#include "String_.h"
#include "Stream.h"
#include "holo_port.h"
#include "Audio_Holo.h"

#define HOLO_AUDIO_RATE         22050
#define HOLO_AUDIO_CHANNELS     1
#define HOLO_AUDIO_BITS         16
#define HOLO_AUDIO_BLOCK_FRAMES 256
#define HOLO_AUDIO_BLOCK_BYTES  (HOLO_AUDIO_BLOCK_FRAMES * sizeof(cc_int16))
#define HOLO_AUDIO_VOICES       8
#define HOLO_AUDIO_STACK_BYTES  (8u * 1024u)
#define HOLO_BGM_BUFFER_FRAMES   4096u
#define HOLO_BGM_REPEAT_DELAY_MS 3000u

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

struct HoloBgmState {
    struct Stream stream;
    cc_uint32 dataOffset, dataBytes, bytesRead;
    cc_uint32 bufferFrames, bufferIndex;
    cc_uint32 sampleRate, phase, resumeAtMs;
    cc_int16 currentSample;
    cc_bool open, haveCurrent;
};

struct HoloEqBiquad {
    int x1, x2, y1, y2;
};

struct HoloEqState {
    struct HoloEqBiquad lowMid;
    struct HoloEqBiquad highMid;
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
static cc_uint32 s_bgm_gain;
static struct HoloBgmState s_bgm;
static cc_int16 s_bgm_buffer[HOLO_BGM_BUFFER_FRAMES];
static struct HoloEqState s_eq;

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

#define WAV_FOURCC(a, b, c, d) (((cc_uint32)(a) << 24) | ((cc_uint32)(b) << 16) | \
                                ((cc_uint32)(c) << 8)  |  (cc_uint32)(d))

static void BgmClose(void) {
    if (s_bgm.open) (void)s_bgm.stream.Close(&s_bgm.stream);
    Mem_Set(&s_bgm, 0, sizeof(s_bgm));
}

static cc_result BgmRewind(void) {
    cc_result res;
    if (!s_bgm.open) return ReturnCode_FileNotFound;
    res = s_bgm.stream.Seek(&s_bgm.stream, s_bgm.dataOffset);
    if (res) return res;
    s_bgm.bytesRead    = 0;
    s_bgm.bufferFrames = 0;
    s_bgm.bufferIndex  = 0;
    s_bgm.phase        = 0;
    s_bgm.resumeAtMs   = 0;
    s_bgm.haveCurrent  = false;
    return 0;
}

static cc_result BgmOpen(void) {
    static const cc_string path = String_FromConst("audio/bgm.wav");
    cc_filepath rawPath;
    cc_uint8 header[16];
    cc_uint32 chunkSize, position;
    cc_bool foundFormat = false;
    cc_result res;
    int i;

    if (s_bgm.open) return 0;
    Platform_EncodePath(&rawPath, &path);
    res = Stream_OpenPath(&s_bgm.stream, &rawPath);
    if (res) {
        Platform_Log1("[cc.audio] WARN: cannot open audio/bgm.wav (%i); BGM disabled", &res);
        return res;
    }
    s_bgm.open = true;

    if ((res = Stream_Read(&s_bgm.stream, header, 12))) goto fail;
    if (Mem_ReadU32_BE(header + 0) != WAV_FOURCC('R','I','F','F')) {
        res = WAV_ERR_STREAM_HDR; goto fail;
    }
    if (Mem_ReadU32_BE(header + 8) != WAV_FOURCC('W','A','V','E')) {
        res = WAV_ERR_STREAM_TYPE; goto fail;
    }

    for (i = 0; i < 32; i++) {
        cc_uint32 fourCC;
        if ((res = Stream_Read(&s_bgm.stream, header, 8))) goto fail;
        fourCC   = Mem_ReadU32_BE(header + 0);
        chunkSize = Mem_ReadU32_LE(header + 4);

        if (fourCC == WAV_FOURCC('f','m','t',' ')) {
            if (chunkSize < 16) { res = WAV_ERR_STREAM_TYPE; goto fail; }
            if ((res = Stream_Read(&s_bgm.stream, header, 16))) goto fail;
            if (Mem_ReadU16_LE(header + 0) != 1 || Mem_ReadU16_LE(header + 2) != 1) {
                res = WAV_ERR_DATA_TYPE; goto fail;
            }
            s_bgm.sampleRate = Mem_ReadU32_LE(header + 4);
            if (Mem_ReadU16_LE(header + 14) != 16) {
                res = WAV_ERR_SAMPLE_BITS; goto fail;
            }
            if (s_bgm.sampleRate != 11025 && s_bgm.sampleRate != HOLO_AUDIO_RATE) {
                res = ERR_NOT_SUPPORTED; goto fail;
            }
            if (chunkSize > 16 && (res = s_bgm.stream.Skip(&s_bgm.stream, chunkSize - 16))) goto fail;
            if ((chunkSize & 1u) && (res = s_bgm.stream.Skip(&s_bgm.stream, 1))) goto fail;
            foundFormat = true;
        } else if (fourCC == WAV_FOURCC('d','a','t','a')) {
            if (!foundFormat || chunkSize < 2) { res = WAV_ERR_STREAM_TYPE; goto fail; }
            if ((res = s_bgm.stream.Position(&s_bgm.stream, &position))) goto fail;
            s_bgm.dataOffset = position;
            s_bgm.dataBytes  = chunkSize & ~1u;
            if ((res = BgmRewind())) goto fail;
            {
                int rate = (int)s_bgm.sampleRate;
                Platform_Log1("[cc.audio] bgm=audio/bgm.wav PCM mono %iHz streamed", &rate);
            }
            return 0;
        } else {
            cc_uint32 skip = chunkSize + (chunkSize & 1u);
            if ((res = s_bgm.stream.Skip(&s_bgm.stream, skip))) goto fail;
        }
    }
    res = WAV_ERR_STREAM_TYPE;

fail:
    Platform_Log1("[cc.audio] WARN: invalid audio/bgm.wav (%i); BGM disabled", &res);
    BgmClose();
    return res;
}

static cc_result BgmRefill(void) {
    cc_uint32 remaining, bytes;
    cc_result res;
    if (!s_bgm.open) return ReturnCode_FileNotFound;
    if (s_bgm.bytesRead >= s_bgm.dataBytes) return ERR_END_OF_STREAM;

    remaining = s_bgm.dataBytes - s_bgm.bytesRead;
    bytes = min(remaining, (cc_uint32)sizeof(s_bgm_buffer));
    bytes &= ~1u;
    if (!bytes) return ERR_END_OF_STREAM;
    if ((res = Stream_Read(&s_bgm.stream, (cc_uint8*)s_bgm_buffer, bytes))) return res;
    s_bgm.bytesRead    += bytes;
    s_bgm.bufferFrames  = bytes / sizeof(cc_int16);
    s_bgm.bufferIndex   = 0;
    return 0;
}

static cc_result BgmReadSourceSample(cc_int16 *sample) {
    cc_result res;
    if (s_bgm.bufferIndex >= s_bgm.bufferFrames) {
        res = BgmRefill();
        if (res) return res;
    }
    *sample = s_bgm_buffer[s_bgm.bufferIndex++];
    return 0;
}

static int BgmNextSample(void) {
    const module_host_api_v2 *host = AudioHost();
    cc_result res;
    int sample = 0;
    if (s_bgm.resumeAtMs) return 0;
    if (!s_bgm.haveCurrent) {
        res = BgmReadSourceSample(&s_bgm.currentSample);
        if (res) goto read_failed;
        s_bgm.haveCurrent = true;
    }
    sample = s_bgm.currentSample;
    s_bgm.phase += s_bgm.sampleRate;
    if (s_bgm.phase >= HOLO_AUDIO_RATE) {
        s_bgm.phase -= HOLO_AUDIO_RATE;
        res = BgmReadSourceSample(&s_bgm.currentSample);
        if (res) goto read_failed;
    }
    return sample;

read_failed:
    s_bgm.haveCurrent = false;
    if (res == ERR_END_OF_STREAM && host && host->time.millis) {
        s_bgm.resumeAtMs = host->time.millis() + HOLO_BGM_REPEAT_DELAY_MS;
        return sample;
    }
    Platform_Log1("[cc.audio] WARN: reading audio/bgm.wav failed (%i); BGM disabled", &res);
    s_bgm_gain = 0;
    BgmClose();
    return 0;
}

static int AudioEqBiquad(int sample, struct HoloEqBiquad *state,
                         int b0, int b1, int b2, int a1, int a2) {
    int accum = b0 * sample + b1 * state->x1 + b2 * state->x2
              - a1 * state->y1 - a2 * state->y2;
    int output = accum >> 11;
    if (output >  32767) output =  32767;
    if (output < -32768) output = -32768;
    state->x2 = state->x1; state->x1 = sample;
    state->y2 = state->y1; state->y1 = output;
    return output;
}

static int AudioEqSample(int sample) {
    /* Two Q11 peaking biquads at 22050 Hz. The first is centred at 316 Hz
     * (roughly the 200-500 Hz region) with +6 dB gain. The second is centred
     * at 1581 Hz (roughly 1-2.5 kHz) with -2 dB gain. Ten 32-bit multiplies per
     * sample are about 220k MAC/s, avoiding floats, FFTs, and 64-bit math. */
    sample = AudioEqBiquad(sample, &s_eq.lowMid,
                          2111, -3953, 1859, -3953, 1922);
    return AudioEqBiquad(sample, &s_eq.highMid,
                         1965, -2963, 1327, -2963, 1244);
}

static cc_bool AudioHasVoices(void) {
    const module_host_api_v2 *host = AudioHost();
    int i;
    cc_bool active;
    if (!AudioLock()) return false;
    active = s_bgm_gain != 0 && s_bgm.open && !s_bgm.resumeAtMs;
    if (s_bgm_gain && s_bgm.open && s_bgm.resumeAtMs && host && host->time.millis &&
        (cc_int32)(host->time.millis() - s_bgm.resumeAtMs) >= 0) {
        cc_result res = BgmRewind();
        if (res) {
            Platform_Log1("[cc.audio] WARN: rewinding audio/bgm.wav failed (%i); BGM disabled", &res);
            s_bgm_gain = 0;
            BgmClose();
        } else {
            active = true;
        }
    }
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
        int mixed = s_bgm_gain ? (BgmNextSample() * (int)s_bgm_gain) >> 8 : 0;
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
        mixed = AudioClampSample(mixed);
        s_audio_mix[frame] = AudioClampSample(AudioEqSample(mixed));
    }

    active = (s_bgm_gain && !s_bgm.resumeAtMs) ? 1u : 0u;
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
    s_bgm_gain = 0;
    BgmClose();
    Mem_Set(&s_eq, 0, sizeof(s_eq));

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

    /* Core 0 is intentional: rendering/gameplay stays on Core 1. Streaming
     * BGM refills enter the host filesystem from this task, so retain 8 KiB
     * of PSRAM stack headroom; 6 KiB was marginal and could corrupt a return
     * address under the deeper SD read call chain. The host retains internal
     * DMA memory ownership. */
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
    holo_port_log("[cc.audio] output=22050Hz S16 mono mixer=8 task=Core0 prio4 stack=8192 PSRAM");
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
    s_bgm_gain = 0;
    BgmClose();

    if (host->sync.destroy) {
        if (s_audio_done) host->sync.destroy(s_audio_done);
        if (s_audio_lock) host->sync.destroy(s_audio_lock);
    }
    s_audio_done = NULL;
    s_audio_lock = NULL;
    s_audio_active_voices = 0;
    Mem_Set(&s_eq, 0, sizeof(s_eq));
    Mem_Set(s_contexts, 0, sizeof(s_contexts));
}

void AudioBackend_Tick(void) { }
void AudioBackend_Free(void) { Audio_HoloShutdown(); }

void Audio_HoloSetMusicVolume(int volume) {
    cc_uint32 gain;
    cc_uint32 oldGain;
    cc_result res = 0;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    /* Keep BGM well behind gameplay: one fifth of the previous 0-200% curve.
     * SFX uses its own volume path and is intentionally unchanged. */
    gain = (cc_uint32)((volume * 512 + 250) / 500);

    if (!s_audio_lock || !AudioLock()) {
        if (!gain) s_bgm_gain = 0;
        return;
    }
    oldGain = s_bgm_gain;
    if (!oldGain && gain) {
        res = BgmOpen();
        if (!res) res = BgmRewind();
        if (res) gain = 0;
    } else if (oldGain && !gain) {
        BgmClose();
    }
    s_bgm_gain = gain;
    AudioUnlock();
}

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
    ctx->volume = 200;
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
    /* UI 0-100 maps to mixer 0-200%: 50 = unity, 100 = 2x. */
    ctx->volume = volume * 2;
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
