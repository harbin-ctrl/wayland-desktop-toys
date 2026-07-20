
#ifndef TOY_AUDIO_H
#define TOY_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#define TOY_AUDIO_SAMPLE_RATE 48000
/* Keep the mixer bounded and preallocated, but leave enough headroom for
 * dense bursts of short effects.  Voices are still recycled when this pool
 * is exhausted; no audio-thread allocations are performed. */
#define TOY_AUDIO_MAX_VOICES 32

#define TOY_AUDIO_VOLUME_MIN 0.0f
#define TOY_AUDIO_VOLUME_MAX 1.5f
#define TOY_AUDIO_VOLUME_STEP 0.05f
#define TOY_AUDIO_DEFAULT_VOLUME 1.2f

#define TOY_AUDIO_NORMALIZE_TARGET 1.0f
#define TOY_AUDIO_CLIP_THRESHOLD 0.9f
#define TOY_AUDIO_QUIET_THRESHOLD 0.004f
#define TOY_AUDIO_QUIET_SECONDS 0.25f

#define TOY_AUDIO_LAYER_DETUNE 0.891f
#define TOY_AUDIO_STEAL_CROSSFADE 96

typedef enum {
    TOY_BOUNCE_POINGO = 0,     
    TOY_BOUNCE_NOSTALGIA = 1,  
} ToyBounceStyle;

typedef struct {
    float *data1;
    float *data2;
    int length;
    bool dirty;
} ToySamplePair;

typedef struct {
    int current_pos;
    bool playing;
    float volume_scale;
    float l_gain, r_gain;   
    float *snapshot_data1;
    float *snapshot_data2;
    int snapshot_length;
    int snapshot_capacity;  
    float steal_tail_l[TOY_AUDIO_STEAL_CROSSFADE];
    float steal_tail_r[TOY_AUDIO_STEAL_CROSSFADE];
    int steal_fade_pos;
} ToyVoice;

typedef struct {
    ToyVoice voices[TOY_AUDIO_MAX_VOICES];
    float master_volume;
    float master_volume_before_mute;
    bool muted;
    bool fading_out;
    float fade_gain;
    float fade_step;    
    float quiet_recent_peak;
    float quiet_seconds;
} ToyMixer;


void toy_mixer_init(ToyMixer *m, float initial_volume);
bool toy_mixer_reserve(ToyMixer *m, int max_samples);

void toy_mixer_render(ToyMixer *m, float *out, int nsamples, bool suppress);

void toy_mixer_render_stereo(ToyMixer *m, float *out, int nframes, bool suppress);

void toy_mixer_start_voice(ToyMixer *m, const ToySamplePair *pair,
                           float volume_scale);

void toy_mixer_start_voice_panned(ToyMixer *m, const ToySamplePair *pair,
                                  float volume_scale,
                                  float l_gain, float r_gain);

bool toy_mixer_any_voice_playing(const ToyMixer *m);

void toy_mixer_adjust_volume(ToyMixer *m, float delta);
void toy_mixer_set_mute(ToyMixer *m, bool mute);

void toy_mixer_begin_fade_out(ToyMixer *m, float seconds);

bool toy_mixer_is_quiet(const ToyMixer *m);

void toy_mixer_reset(ToyMixer *m);

/* Reserve the temporary synthesis workspace before audio starts. */
bool toy_audio_reserve_scratch(int max_samples);
void toy_audio_release_scratch(void);


bool toy_sample_pair_alloc(ToySamplePair *pair, int length);
void toy_sample_pair_free(ToySamplePair *pair);


float toy_audio_find_peak(const float *buffer, int nsamples);
void toy_audio_normalize(float *buffer, int nsamples, float target_peak);

void toy_audio_micro_reverb(float *buffer, int nsamples, float time_scale,
                            float rt60, float wet_mix);

void toy_audio_generate_bounce(float *buffer, int nsamples, int pitch,
                               int volume, float decay_time,
                               float noise_amount, float resonance_amount,
                               ToyBounceStyle style);

void toy_audio_generate_bounce_pair(ToySamplePair *pair, float size_scale,
                                    ToyBounceStyle style);

int toy_audio_bounce_pair_length(void);

void toy_audio_equal_power_pan(float pan, float *l_gain, float *r_gain);


#define TOY_HISS_IDLE_ENV 0.0005f

typedef struct {
    uint32_t noise;     
    float env;          
    float burst;        
    float lowpass;      
    float lfo_phase;    
    bool prev_active;   
} ToyHiss;

void toy_hiss_init(ToyHiss *h);

void toy_hiss_render(ToyHiss *h, float *out, int nsamples,
                     bool active, float size01, int sample_rate);

bool toy_hiss_is_idle(const ToyHiss *h);

#endif // TOY_AUDIO_H
