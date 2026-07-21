#define _GNU_SOURCE
#include "toy_audio_stream.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <pipewire/pipewire.h>
#include <pipewire/core.h>
#include <pipewire/keys.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

#define TOY_AUDIO_STREAM_NAME_MAX 64
#define TOY_AUDIO_STREAM_DESCRIPTION_MAX 128
#define TOY_AUDIO_NSEC_PER_SEC 1000000000ULL
#define TOY_AUDIO_START_TIMEOUT_SEC 5
#define TOY_AUDIO_GRAPH_PORTS_MAX 32
#define TOY_AUDIO_GRAPH_LINKS_MAX 16

typedef struct {
    uint32_t id;
    uint32_t node_id;
    char channel[16];
    char name[64];
    bool input;
    bool present;
} ToyAudioGraphPort;

typedef struct {
    uint32_t id;
    uint32_t output_node;
    uint32_t output_port;
    uint32_t input_node;
    uint32_t input_port;
    bool present;
    bool reported;
} ToyAudioGraphLink;

struct ToyAudioStream {
    struct pw_thread_loop *loop;
    struct pw_stream *stream;
    ToyAudioRenderCallback render;
    void *userdata;
    uint32_t sample_rate;
    uint32_t channels;
    char name[TOY_AUDIO_STREAM_NAME_MAX];
    char description[TOY_AUDIO_STREAM_DESCRIPTION_MAX];
    bool pipewire_initialized;
    atomic_bool streaming;
    atomic_bool format_valid;
    atomic_bool startup_failed;
    atomic_uint_fast64_t latency_ns;
    struct pw_registry *registry;
    struct spa_hook registry_listener;
    uint32_t stream_node_id;
    ToyAudioGraphPort graph_ports[TOY_AUDIO_GRAPH_PORTS_MAX];
    ToyAudioGraphLink graph_links[TOY_AUDIO_GRAPH_LINKS_MAX];
};

static bool stream_format_matches(const ToyAudioStream *audio,
                                  const struct spa_audio_info_raw *info) {
    if (!audio || !info ||
        info->format != SPA_AUDIO_FORMAT_F32 ||
        info->rate != audio->sample_rate ||
        info->channels != audio->channels) {
        return false;
    }
    if (audio->channels == 1) {
        return info->position[0] == SPA_AUDIO_CHANNEL_MONO;
    }
    return info->position[0] == SPA_AUDIO_CHANNEL_FL &&
           info->position[1] == SPA_AUDIO_CHANNEL_FR;
}

static const char *channel_name(uint32_t channel) {
    switch (channel) {
    case SPA_AUDIO_CHANNEL_MONO: return "MONO";
    case SPA_AUDIO_CHANNEL_FL: return "FL";
    case SPA_AUDIO_CHANNEL_FR: return "FR";
    case SPA_AUDIO_CHANNEL_FC: return "FC";
    case SPA_AUDIO_CHANNEL_LFE: return "LFE";
    case SPA_AUDIO_CHANNEL_RL: return "RL";
    case SPA_AUDIO_CHANNEL_RR: return "RR";
    case SPA_AUDIO_CHANNEL_SL: return "SL";
    case SPA_AUDIO_CHANNEL_SR: return "SR";
    default: return "UNKNOWN";
    }
}

static void report_format(const ToyAudioStream *audio,
                          const struct spa_audio_info_raw *info,
                          bool valid) {
    uint32_t first = info ? info->position[0] : 0;
    uint32_t second = info && info->channels > 1 ? info->position[1] : 0;
    fprintf(stderr,
            "%s: PipeWire negotiated %s format=%u rate=%u channels=%u "
            "positions=[%s%s%s]\n",
            audio->name, valid ? "audio" : "unexpected audio",
            info ? info->format : 0, info ? info->rate : 0,
            info ? info->channels : 0, info ? channel_name(first) : "none",
            info && info->channels > 1 ? "," : "",
            info && info->channels > 1 ? channel_name(second) : "");
}

static void signal_startup_change(ToyAudioStream *audio) {
    if (audio && audio->loop) {
        pw_thread_loop_signal(audio->loop, false);
    }
}

static uint32_t property_id(const struct spa_dict *props, const char *key) {
    const char *value = props ? spa_dict_lookup(props, key) : NULL;
    if (!value || !*value) {
        return PW_ID_ANY;
    }
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    return end && *end == '\0' && parsed <= UINT32_MAX
               ? (uint32_t)parsed
               : PW_ID_ANY;
}

static void copy_property(char *destination, size_t size,
                          const struct spa_dict *props, const char *key) {
    const char *value = props ? spa_dict_lookup(props, key) : NULL;
    snprintf(destination, size, "%s", value ? value : "?");
}

static ToyAudioGraphPort *find_graph_port(ToyAudioStream *audio, uint32_t id) {
    for (size_t i = 0; i < TOY_AUDIO_GRAPH_PORTS_MAX; ++i) {
        if (audio->graph_ports[i].present && audio->graph_ports[i].id == id) {
            return &audio->graph_ports[i];
        }
    }
    return NULL;
}

static ToyAudioGraphLink *find_graph_link(ToyAudioStream *audio, uint32_t id) {
    for (size_t i = 0; i < TOY_AUDIO_GRAPH_LINKS_MAX; ++i) {
        if (audio->graph_links[i].present && audio->graph_links[i].id == id) {
            return &audio->graph_links[i];
        }
    }
    return NULL;
}

static void audit_graph(ToyAudioStream *audio) {
    for (size_t i = 0; i < TOY_AUDIO_GRAPH_LINKS_MAX; ++i) {
        ToyAudioGraphLink *link = &audio->graph_links[i];
        if (!link->present || link->reported ||
            link->output_node != audio->stream_node_id) {
            continue;
        }
        ToyAudioGraphPort *source = find_graph_port(audio, link->output_port);
        ToyAudioGraphPort *destination = find_graph_port(audio, link->input_port);
        if (!source || !destination) {
            continue;
        }
        fprintf(stderr,
                "%s: PipeWire route %s/%s -> %s/%s (%s -> %s)\n",
                audio->name, source->name, source->channel,
                destination->name, destination->channel,
                source->input ? "input" : "output",
                destination->input ? "input" : "output");
        bool mono_upmix = audio->channels == 1 &&
                          strcmp(source->channel, "MONO") == 0 &&
                          (strcmp(destination->channel, "FL") == 0 ||
                           strcmp(destination->channel, "FR") == 0);
        if (!mono_upmix && strcmp(source->channel, destination->channel) != 0) {
            fprintf(stderr,
                    "%s: ERROR: PipeWire changed channel route %s -> %s\n",
                    audio->name, source->channel, destination->channel);
            atomic_store_explicit(&audio->startup_failed, true,
                                  memory_order_release);
        }
        link->reported = true;
        signal_startup_change(audio);
    }
}

static void graph_global(void *userdata, uint32_t id, uint32_t permissions,
                         const char *type, uint32_t version,
                         const struct spa_dict *props) {
    ToyAudioStream *audio = userdata;
    (void)permissions;
    (void)version;
    if (!audio || !type || !props) {
        return;
    }
    if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
        ToyAudioGraphPort *port = find_graph_port(audio, id);
        if (!port) {
            for (size_t i = 0; i < TOY_AUDIO_GRAPH_PORTS_MAX; ++i) {
                if (!audio->graph_ports[i].present) {
                    port = &audio->graph_ports[i];
                    break;
                }
            }
        }
        if (!port) {
            fprintf(stderr, "%s: PipeWire graph port table is full\n",
                    audio->name);
            return;
        }
        port->id = id;
        port->node_id = property_id(props, PW_KEY_NODE_ID);
        copy_property(port->channel, sizeof(port->channel), props,
                      PW_KEY_AUDIO_CHANNEL);
        copy_property(port->name, sizeof(port->name), props,
                      PW_KEY_PORT_NAME);
        const char *direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
        port->input = direction && strcmp(direction, "in") == 0;
        port->present = true;
    } else if (strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
        ToyAudioGraphLink *link = find_graph_link(audio, id);
        if (!link) {
            for (size_t i = 0; i < TOY_AUDIO_GRAPH_LINKS_MAX; ++i) {
                if (!audio->graph_links[i].present) {
                    link = &audio->graph_links[i];
                    break;
                }
            }
        }
        if (!link) {
            fprintf(stderr, "%s: PipeWire graph link table is full\n",
                    audio->name);
            return;
        }
        link->id = id;
        link->output_node = property_id(props, "link.output.node");
        link->output_port = property_id(props, "link.output.port");
        link->input_node = property_id(props, "link.input.node");
        link->input_port = property_id(props, "link.input.port");
        link->reported = false;
        link->present = true;
    }
    audit_graph(audio);
}

static void graph_global_remove(void *userdata, uint32_t id) {
    ToyAudioStream *audio = userdata;
    if (!audio) {
        return;
    }
    ToyAudioGraphPort *port = find_graph_port(audio, id);
    if (port) {
        port->present = false;
        return;
    }
    ToyAudioGraphLink *link = find_graph_link(audio, id);
    if (link) {
        link->present = false;
    }
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = graph_global,
    .global_remove = graph_global_remove,
};

static void publish_latency(ToyAudioStream *audio) {
    struct pw_time time_info;
    if (!audio || !audio->stream ||
        pw_stream_get_time_n(audio->stream, &time_info,
                             sizeof(time_info)) < 0 ||
        time_info.rate.num == 0 || time_info.rate.denom == 0) {
        return;
    }

    int64_t now_ns = (int64_t)pw_stream_get_nsec(audio->stream);
    int64_t diff_ns =
        now_ns > time_info.now ? now_ns - time_info.now : 0;
    int64_t elapsed_ticks =
        (int64_t)(((uint64_t)time_info.rate.denom *
                   (uint64_t)diff_ns) /
                  ((uint64_t)time_info.rate.num *
                   TOY_AUDIO_NSEC_PER_SEC));
    int64_t graph_delay_ticks = time_info.delay - elapsed_ticks;
    if (graph_delay_ticks < 0) {
        graph_delay_ticks = 0;
    }

    double latency_seconds =
        (double)time_info.queued / (double)audio->sample_rate +
        (double)time_info.buffered / (double)audio->sample_rate +
        (double)graph_delay_ticks * (double)time_info.rate.num /
            (double)time_info.rate.denom;
    if (latency_seconds < 0.0) {
        latency_seconds = 0.0;
    }

    atomic_store_explicit(
        &audio->latency_ns,
        (uint64_t)llround(latency_seconds *
                          (double)TOY_AUDIO_NSEC_PER_SEC),
        memory_order_release);
}

static void stream_process(void *userdata) {
    ToyAudioStream *audio = userdata;
    if (!audio || !audio->stream) {
        return;
    }

    struct pw_buffer *pw_buffer =
        pw_stream_dequeue_buffer(audio->stream);
    if (!pw_buffer) {
        return;
    }
    if (!pw_buffer->buffer || pw_buffer->buffer->n_datas < 1) {
        pw_stream_return_buffer(audio->stream, pw_buffer);
        return;
    }

    struct spa_data *data = &pw_buffer->buffer->datas[0];
    struct spa_chunk *chunk = data->chunk;
    uint32_t stride = audio->channels * (uint32_t)sizeof(float);
    uint32_t nframes = stride > 0 ? data->maxsize / stride : 0;
    if (pw_buffer->requested > 0 &&
        pw_buffer->requested < nframes) {
        nframes = (uint32_t)pw_buffer->requested;
    }

    publish_latency(audio);
    if (data->data && nframes > 0 &&
        atomic_load_explicit(&audio->format_valid,
                             memory_order_acquire)) {
        audio->render(audio->userdata, data->data, nframes,
                      audio->channels);
    } else if (data->data && nframes > 0) {
        memset(data->data, 0, (size_t)nframes * stride);
    }

    if (!data->data) {
        nframes = 0;
    }
    pw_buffer->size = nframes;
    if (chunk) {
        chunk->offset = 0;
        chunk->stride = (int32_t)stride;
        chunk->size = nframes * stride;
    }
    pw_stream_queue_buffer(audio->stream, pw_buffer);
}

static void stream_param_changed(void *userdata, uint32_t id,
                                 const struct spa_pod *param) {
    ToyAudioStream *audio = userdata;
    if (!audio || id != SPA_PARAM_Format) {
        return;
    }
    if (!param) {
        atomic_store_explicit(&audio->format_valid, false,
                              memory_order_release);
        atomic_store_explicit(&audio->startup_failed, true,
                              memory_order_release);
        fprintf(stderr, "%s: PipeWire removed the negotiated audio format\n",
                audio->name);
        signal_startup_change(audio);
        return;
    }

    struct spa_audio_info_raw info = {0};
    bool valid =
        spa_format_audio_raw_parse(param, &info) >= 0 &&
        stream_format_matches(audio, &info);
    atomic_store_explicit(&audio->format_valid, valid,
                          memory_order_release);
    report_format(audio, &info, valid);
    if (!valid) {
        atomic_store_explicit(&audio->startup_failed, true,
                              memory_order_release);
    }
    signal_startup_change(audio);
}

static void stream_state_changed(void *userdata,
                                 enum pw_stream_state old_state,
                                 enum pw_stream_state state,
                                 const char *error) {
    ToyAudioStream *audio = userdata;
    (void)old_state;
    if (!audio) {
        return;
    }
    atomic_store_explicit(&audio->streaming,
                          state == PW_STREAM_STATE_STREAMING,
                          memory_order_release);
    if (state == PW_STREAM_STATE_STREAMING && audio->stream) {
        audio->stream_node_id = pw_stream_get_node_id(audio->stream);
        audit_graph(audio);
    }
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "%s: PipeWire stream error: %s\n",
                audio->name, error ? error : "unknown error");
        atomic_store_explicit(&audio->startup_failed, true,
                              memory_order_release);
    }
    signal_startup_change(audio);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = stream_state_changed,
    .param_changed = stream_param_changed,
    .process = stream_process,
};

static void destroy_stream(ToyAudioStream *audio) {
    if (!audio) {
        return;
    }
    if (audio->loop) {
        pw_thread_loop_stop(audio->loop);
    }
    if (audio->stream) {
        pw_stream_destroy(audio->stream);
        audio->stream = NULL;
    }
    if (audio->registry) {
        spa_hook_remove(&audio->registry_listener);
        pw_proxy_destroy((struct pw_proxy *)audio->registry);
        audio->registry = NULL;
    }
    if (audio->loop) {
        pw_thread_loop_destroy(audio->loop);
        audio->loop = NULL;
    }
    if (audio->pipewire_initialized) {
        pw_deinit();
        audio->pipewire_initialized = false;
    }
    atomic_store_explicit(&audio->streaming, false,
                          memory_order_release);
    atomic_store_explicit(&audio->format_valid, false,
                          memory_order_release);
    atomic_store_explicit(&audio->startup_failed, false,
                          memory_order_release);
    atomic_store_explicit(&audio->latency_ns, 0,
                          memory_order_release);
}

ToyAudioStream *toy_audio_stream_start(
    const ToyAudioStreamConfig *config) {
    if (!config || !config->name || !config->render ||
        config->sample_rate == 0 ||
        (config->channels != 1 && config->channels != 2)) {
        return NULL;
    }

    ToyAudioStream *audio = calloc(1, sizeof(*audio));
    if (!audio) {
        return NULL;
    }
    audio->render = config->render;
    audio->userdata = config->userdata;
    audio->sample_rate = config->sample_rate;
    audio->channels = config->channels;
    snprintf(audio->name, sizeof(audio->name), "%s", config->name);
    snprintf(audio->description, sizeof(audio->description), "%s",
             config->description ? config->description : config->name);
    atomic_init(&audio->streaming, false);
    atomic_init(&audio->format_valid, false);
    atomic_init(&audio->startup_failed, false);
    atomic_init(&audio->latency_ns, 0);

    pw_init(NULL, NULL);
    audio->pipewire_initialized = true;

    char loop_name[TOY_AUDIO_STREAM_NAME_MAX + sizeof("-audio")];
    snprintf(loop_name, sizeof(loop_name), "%s-audio", audio->name);
    audio->loop = pw_thread_loop_new(loop_name, NULL);
    if (!audio->loop) {
        fprintf(stderr, "%s: failed to create PipeWire audio loop\n",
                audio->name);
        toy_audio_stream_stop(audio);
        return NULL;
    }

    char rate[16];
    char channels[8];
    const char *position =
        audio->channels == 1 ? "[ MONO ]" : "[ FL FR ]";
    snprintf(rate, sizeof(rate), "%u", audio->sample_rate);
    snprintf(channels, sizeof(channels), "%u", audio->channels);
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Game",
        PW_KEY_NODE_NAME, audio->name,
        PW_KEY_NODE_DESCRIPTION, audio->description,
        SPA_KEY_AUDIO_FORMAT, "F32LE",
        SPA_KEY_AUDIO_RATE, rate,
        SPA_KEY_AUDIO_CHANNELS, channels,
        SPA_KEY_AUDIO_POSITION, position,
        NULL);
    if (!props) {
        fprintf(stderr, "%s: failed to create PipeWire properties\n",
                audio->name);
        toy_audio_stream_stop(audio);
        return NULL;
    }

    audio->stream = pw_stream_new_simple(
        pw_thread_loop_get_loop(audio->loop), audio->name,
        props, &stream_events, audio);
    if (!audio->stream) {
        fprintf(stderr, "%s: failed to create PipeWire stream\n",
                audio->name);
        toy_audio_stream_stop(audio);
        return NULL;
    }

    uint8_t pod_buffer[1024];
    struct spa_pod_builder builder =
        SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
    struct spa_audio_info_raw format = {
        .format = SPA_AUDIO_FORMAT_F32,
        .rate = audio->sample_rate,
        .channels = audio->channels,
    };
    format.position[0] = audio->channels == 1
                             ? SPA_AUDIO_CHANNEL_MONO
                             : SPA_AUDIO_CHANNEL_FL;
    if (audio->channels == 2) {
        format.position[1] = SPA_AUDIO_CHANNEL_FR;
    }

    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(
        &builder, SPA_PARAM_EnumFormat, &format);
    int result = pw_stream_connect(
        audio->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        PW_STREAM_FLAG_AUTOCONNECT |
            PW_STREAM_FLAG_MAP_BUFFERS |
            PW_STREAM_FLAG_RT_PROCESS,
        params, 1);
    if (result < 0) {
        fprintf(stderr, "%s: failed to connect PipeWire stream: %s\n",
                audio->name, spa_strerror(result));
        toy_audio_stream_stop(audio);
        return NULL;
    }

    audio->stream_node_id = pw_stream_get_node_id(audio->stream);
    audio->registry = pw_core_get_registry(
        pw_stream_get_core(audio->stream), PW_VERSION_REGISTRY, 0);
    if (!audio->registry ||
        pw_registry_add_listener(audio->registry, &audio->registry_listener,
                                 &registry_events, audio) < 0) {
        fprintf(stderr, "%s: failed to inspect PipeWire audio graph\n",
                audio->name);
        toy_audio_stream_stop(audio);
        return NULL;
    }

    result = pw_thread_loop_start(audio->loop);
    if (result < 0) {
        fprintf(stderr, "%s: failed to start PipeWire audio loop: %s\n",
                audio->name, spa_strerror(result));
        toy_audio_stream_stop(audio);
        return NULL;
    }

    pw_thread_loop_lock(audio->loop);
    while (!atomic_load_explicit(&audio->startup_failed,
                                 memory_order_acquire) &&
           !toy_audio_stream_is_ready(audio)) {
        if (pw_thread_loop_timed_wait(audio->loop,
                                      TOY_AUDIO_START_TIMEOUT_SEC) < 0) {
            fprintf(stderr, "%s: timed out waiting for valid PipeWire audio\n",
                    audio->name);
            atomic_store_explicit(&audio->startup_failed, true,
                                  memory_order_release);
            break;
        }
    }
    bool ready = toy_audio_stream_is_ready(audio);
    bool failed = atomic_load_explicit(&audio->startup_failed,
                                       memory_order_acquire);
    pw_thread_loop_unlock(audio->loop);
    if (!ready || failed) {
        fprintf(stderr, "%s: refusing to run with an invalid audio format\n",
                audio->name);
        toy_audio_stream_stop(audio);
        return NULL;
    }
    return audio;
}

void toy_audio_stream_stop(ToyAudioStream *audio) {
    if (!audio) {
        return;
    }
    destroy_stream(audio);
    free(audio);
}

bool toy_audio_stream_get_latency(const ToyAudioStream *audio,
                                  double *seconds) {
    if (!audio || !seconds ||
        !atomic_load_explicit(&audio->streaming,
                              memory_order_acquire) ||
        !atomic_load_explicit(&audio->format_valid,
                              memory_order_acquire)) {
        return false;
    }
    uint64_t latency_ns = atomic_load_explicit(
        &audio->latency_ns, memory_order_acquire);
    if (latency_ns == 0) {
        return false;
    }
    *seconds = (double)latency_ns / (double)TOY_AUDIO_NSEC_PER_SEC;
    return true;
}

bool toy_audio_stream_is_ready(const ToyAudioStream *audio) {
    return audio &&
           atomic_load_explicit(&audio->streaming,
                                memory_order_acquire) &&
           atomic_load_explicit(&audio->format_valid,
                                memory_order_acquire);
}
