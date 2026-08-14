/*
 * Android AAudio audio backend
 *
 * Adapted from the trimmed-build AAudio backend for QEMU 11.0.50
 * (QOM AudioMixengBackend style).
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/audio.h"
#include "qom/object.h"

#include "audio_int.h"
#include <aaudio/AAudio.h>

#define TYPE_AUDIO_AAUDIO "audio-aaudio"
OBJECT_DECLARE_SIMPLE_TYPE(AudioAAudio, AUDIO_AAUDIO)

struct AudioAAudio {
    AudioMixengBackend parent_obj;
};

typedef struct AAudioVoiceOut {
    HWVoiceOut hw;
    AAudioStream *stream;
} AAudioVoiceOut;

typedef struct AAudioVoiceIn {
    HWVoiceIn hw;
    AAudioStream *stream;
} AAudioVoiceIn;

static aaudio_format_t qemu_to_aaudio_fmt(AudioFormat fmt)
{
    switch (fmt) {
    case AUDIO_FORMAT_S16:
        return AAUDIO_FORMAT_PCM_I16;
    case AUDIO_FORMAT_F32:
        return AAUDIO_FORMAT_PCM_FLOAT;
    default:
        error_report("aaudio: unsupported format %d, falling back to S16", fmt);
        return AAUDIO_FORMAT_PCM_I16;
    }
}

static AAudioStream *aaudio_open_stream(struct audsettings *as,
                                        aaudio_direction_t direction)
{
    AAudioStreamBuilder *builder = NULL;
    AAudioStream *stream = NULL;
    aaudio_result_t res;

    res = AAudio_createStreamBuilder(&builder);
    if (res != AAUDIO_OK) {
        error_report("aaudio: AAudio_createStreamBuilder failed: %d", res);
        return NULL;
    }

    AAudioStreamBuilder_setDirection(builder, direction);
    AAudioStreamBuilder_setSampleRate(builder, as->freq);
    AAudioStreamBuilder_setChannelCount(builder, as->nchannels);
    AAudioStreamBuilder_setFormat(builder, qemu_to_aaudio_fmt(as->fmt));
    AAudioStreamBuilder_setPerformanceMode(builder,
                                           AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setBufferCapacityInFrames(builder, 1024 * 2);

    res = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);

    if (res != AAUDIO_OK) {
        error_report("aaudio: AAudioStreamBuilder_openStream failed: %d", res);
        return NULL;
    }

    return stream;
}

static int aaudio_init_out(HWVoiceOut *hw, struct audsettings *as)
{
    AAudioVoiceOut *aa = (AAudioVoiceOut *)hw;

    as->fmt = AUDIO_FORMAT_S16;

    aa->stream = aaudio_open_stream(as, AAUDIO_DIRECTION_OUTPUT);
    if (!aa->stream) {
        return -1;
    }

    audio_pcm_init_info(&hw->info, as);
    hw->samples = AAudioStream_getBufferSizeInFrames(aa->stream);

    return 0;
}

static void aaudio_fini_out(HWVoiceOut *hw)
{
    AAudioVoiceOut *aa = (AAudioVoiceOut *)hw;

    if (aa->stream) {
        AAudioStream_requestStop(aa->stream);
        AAudioStream_close(aa->stream);
        aa->stream = NULL;
    }
}

static size_t aaudio_write(HWVoiceOut *hw, void *buf, size_t len)
{
    AAudioVoiceOut *aa = (AAudioVoiceOut *)hw;
    int frames = len / hw->info.bytes_per_frame;
    aaudio_result_t written;

    if (!aa->stream) {
        return 0;
    }

    written = AAudioStream_write(aa->stream, buf, frames, 0);
    if (written < 0) {
        error_report("aaudio: AAudioStream_write failed: %d", written);
        return 0;
    }

    return written * hw->info.bytes_per_frame;
}

static void aaudio_enable_out(HWVoiceOut *hw, bool enable)
{
    AAudioVoiceOut *aa = (AAudioVoiceOut *)hw;

    if (!aa->stream) {
        return;
    }

    if (enable) {
        AAudioStream_requestStart(aa->stream);
    } else {
        AAudioStream_requestPause(aa->stream);
    }
}

static int aaudio_init_in(HWVoiceIn *hw, struct audsettings *as)
{
    AAudioVoiceIn *aa = (AAudioVoiceIn *)hw;

    as->fmt = AUDIO_FORMAT_S16;

    aa->stream = aaudio_open_stream(as, AAUDIO_DIRECTION_INPUT);
    if (!aa->stream) {
        return -1;
    }

    audio_pcm_init_info(&hw->info, as);
    hw->samples = AAudioStream_getBufferSizeInFrames(aa->stream);

    return 0;
}

static void aaudio_fini_in(HWVoiceIn *hw)
{
    AAudioVoiceIn *aa = (AAudioVoiceIn *)hw;

    if (aa->stream) {
        AAudioStream_requestStop(aa->stream);
        AAudioStream_close(aa->stream);
        aa->stream = NULL;
    }
}

static size_t aaudio_read(HWVoiceIn *hw, void *buf, size_t len)
{
    AAudioVoiceIn *aa = (AAudioVoiceIn *)hw;
    int frames = len / hw->info.bytes_per_frame;
    aaudio_result_t nread;

    if (!aa->stream) {
        return 0;
    }

    nread = AAudioStream_read(aa->stream, buf, frames, 0);
    if (nread < 0) {
        error_report("aaudio: AAudioStream_read failed: %d", nread);
        return 0;
    }

    return nread * hw->info.bytes_per_frame;
}

static void aaudio_enable_in(HWVoiceIn *hw, bool enable)
{
    AAudioVoiceIn *aa = (AAudioVoiceIn *)hw;

    if (!aa->stream) {
        return;
    }

    if (enable) {
        AAudioStream_requestStart(aa->stream);
    } else {
        AAudioStream_requestStop(aa->stream);
    }
}

static void audio_aaudio_class_init(ObjectClass *klass, const void *data)
{
    AudioMixengBackendClass *k = AUDIO_MIXENG_BACKEND_CLASS(klass);

    k->max_voices_out = 1;
    k->max_voices_in = 1;
    k->voice_size_out = sizeof(AAudioVoiceOut);
    k->voice_size_in = sizeof(AAudioVoiceIn);

    k->init_out = aaudio_init_out;
    k->fini_out = aaudio_fini_out;
    k->write = aaudio_write;
    k->buffer_get_free = audio_generic_buffer_get_free;
    k->run_buffer_out = audio_generic_run_buffer_out;
    k->enable_out = aaudio_enable_out;

    k->init_in = aaudio_init_in;
    k->fini_in = aaudio_fini_in;
    k->read = aaudio_read;
    k->run_buffer_in = audio_generic_run_buffer_in;
    k->enable_in = aaudio_enable_in;
}

static const TypeInfo audio_types[] = {
    {
        .name = TYPE_AUDIO_AAUDIO,
        .parent = TYPE_AUDIO_MIXENG_BACKEND,
        .instance_size = sizeof(AudioAAudio),
        .class_init = audio_aaudio_class_init,
    },
};

DEFINE_TYPES(audio_types)
module_obj(TYPE_AUDIO_AAUDIO);
