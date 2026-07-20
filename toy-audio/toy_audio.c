
#include "toy_audio.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TA_PI 3.14159265358979323846f
#define TA_RATE ((float)TOY_AUDIO_SAMPLE_RATE)

#define TA_BOUNCE_MAX_DECAY 0.9f
#define TA_BOUNCE_ATTACK 0.005f
#define TA_BOUNCE_TAIL_MULTIPLIER 2.5f
#define TA_BOUNCE_BUFFER_SLACK 2048

static float *g_nostalgia_dry;
static int g_nostalgia_dry_capacity;


void toy_mixer_init(ToyMixer *m, float initial_volume) {
    if (!m) return;
    memset(m, 0, sizeof(*m));
    m->master_volume = initial_volume;
    m->master_volume_before_mute = initial_volume;
    m->fade_gain = 1.0f;
    m->limiter_gain = 1.0f;
}

bool toy_mixer_reserve(ToyMixer *m, int max_samples) {
    if (!m || max_samples <= 0) return false;
    for (int v = 0; v < TOY_AUDIO_MAX_VOICES; ++v) {
        ToyVoice *voice = &m->voices[v];
        if (voice->snapshot_capacity >= max_samples) continue;
        float *data1 = malloc((size_t)max_samples * sizeof(float));
        float *data2 = malloc((size_t)max_samples * sizeof(float));
        if (!data1 || !data2) {
            free(data1);
            free(data2);
            for (int i = 0; i <= v; ++i) {
                free(m->voices[i].snapshot_data1);
                free(m->voices[i].snapshot_data2);
                m->voices[i].snapshot_data1 = NULL;
                m->voices[i].snapshot_data2 = NULL;
                m->voices[i].snapshot_capacity = 0;
            }
            return false;
        }
        free(voice->snapshot_data1);
        free(voice->snapshot_data2);
        voice->snapshot_data1 = data1;
        voice->snapshot_data2 = data2;
        voice->snapshot_capacity = max_samples;
    }
    return true;
}

void toy_mixer_render(ToyMixer *m, float *out, int nsamples, bool suppress) {
    if (!m || !out || nsamples <= 0) return;

    float master = (m->muted || suppress) ? 0.0f : m->master_volume;
    float gain = master * m->fade_gain;
    float peak = 0.0f;
    const float limiter_release =
        1.0f / (TOY_AUDIO_LIMITER_RELEASE_SECONDS * TA_RATE);

    for (int i = 0; i < nsamples; i++) {
        float sample = 0.0f;

        for (int v = 0; v < TOY_AUDIO_MAX_VOICES; ++v) {
            ToyVoice *voice = &m->voices[v];
            if (voice->playing &&
                voice->snapshot_data1 &&
                voice->snapshot_data2 &&
                voice->current_pos < voice->snapshot_length) {
                float contribution =
                    (voice->snapshot_data1[voice->current_pos] +
                     voice->snapshot_data2[voice->current_pos]) * 0.5f *
                    voice->volume_scale;
                if (voice->steal_fade_pos < TOY_AUDIO_STEAL_CROSSFADE) {
                    float t = (float)(voice->steal_fade_pos + 1) /
                              (float)TOY_AUDIO_STEAL_CROSSFADE;
                    sample +=
                        voice->steal_tail_l[voice->steal_fade_pos] *
                            (1.0f - t) +
                        contribution * t;
                    voice->steal_fade_pos++;
                } else {
                    sample += contribution;
                }
                voice->current_pos++;
            } else if (voice->preparing &&
                       voice->steal_fade_pos <
                           TOY_AUDIO_STEAL_CROSSFADE) {
                float t = (float)(voice->steal_fade_pos + 1) /
                          (float)TOY_AUDIO_STEAL_CROSSFADE;
                sample += voice->steal_tail_l[voice->steal_fade_pos] *
                          (1.0f - t);
                voice->steal_fade_pos++;
            } else if (!voice->preparing &&
                       voice->current_pos >= voice->snapshot_length) {
                voice->playing = false;
                voice->current_pos = 0;
            }
        }

        if (m->fading_out) {
            m->fade_gain -= m->fade_step;
            if (m->fade_gain < 0.0f) m->fade_gain = 0.0f;
            gain = master * m->fade_gain;
        }

        float mixed = sample * gain;
        float mixed_abs = fabsf(mixed);
        float limiter_target =
            mixed_abs > TOY_AUDIO_CLIP_THRESHOLD
                ? TOY_AUDIO_CLIP_THRESHOLD / mixed_abs
                : 1.0f;
        if (limiter_target < m->limiter_gain) {
            m->limiter_gain = limiter_target;
        } else {
            m->limiter_gain +=
                (1.0f - m->limiter_gain) * limiter_release;
            if (m->limiter_gain > limiter_target) {
                m->limiter_gain = limiter_target;
            }
        }
        mixed *= m->limiter_gain;
        out[i] = mixed;
        float abs_val = fabsf(mixed);
        if (abs_val > peak) {
            peak = abs_val;
        }
    }

    m->quiet_recent_peak = peak;
    if (peak <= TOY_AUDIO_QUIET_THRESHOLD) {
        m->quiet_seconds += (float)nsamples / TA_RATE;
        if (m->quiet_seconds > TOY_AUDIO_QUIET_SECONDS) {
            m->quiet_seconds = TOY_AUDIO_QUIET_SECONDS;
        }
    } else {
        m->quiet_seconds = 0.0f;
    }
}

void toy_mixer_render_stereo(ToyMixer *m, float *out, int nframes,
                             bool suppress) {
    if (!m || !out || nframes <= 0) return;

    float master = (m->muted || suppress) ? 0.0f : m->master_volume;
    float gain = master * m->fade_gain;
    float peak = 0.0f;
    const float limiter_release =
        1.0f / (TOY_AUDIO_LIMITER_RELEASE_SECONDS * TA_RATE);

    for (int i = 0; i < nframes; i++) {
        float left = 0.0f;
        float right = 0.0f;

        for (int v = 0; v < TOY_AUDIO_MAX_VOICES; ++v) {
            ToyVoice *voice = &m->voices[v];
            if (voice->playing &&
                voice->snapshot_data1 &&
                voice->snapshot_data2 &&
                voice->current_pos < voice->snapshot_length) {
                float contribution =
                    (voice->snapshot_data1[voice->current_pos] +
                     voice->snapshot_data2[voice->current_pos]) * 0.5f *
                    voice->volume_scale;
                float new_left = contribution * voice->l_gain;
                float new_right = contribution * voice->r_gain;
                if (voice->steal_fade_pos < TOY_AUDIO_STEAL_CROSSFADE) {
                    float t = (float)(voice->steal_fade_pos + 1) /
                              (float)TOY_AUDIO_STEAL_CROSSFADE;
                    left +=
                        voice->steal_tail_l[voice->steal_fade_pos] *
                            (1.0f - t) +
                        new_left * t;
                    right +=
                        voice->steal_tail_r[voice->steal_fade_pos] *
                            (1.0f - t) +
                        new_right * t;
                    voice->steal_fade_pos++;
                } else {
                    left += new_left;
                    right += new_right;
                }
                voice->current_pos++;
            } else if (voice->preparing &&
                       voice->steal_fade_pos <
                           TOY_AUDIO_STEAL_CROSSFADE) {
                float t = (float)(voice->steal_fade_pos + 1) /
                          (float)TOY_AUDIO_STEAL_CROSSFADE;
                left += voice->steal_tail_l[voice->steal_fade_pos] *
                        (1.0f - t);
                right += voice->steal_tail_r[voice->steal_fade_pos] *
                         (1.0f - t);
                voice->steal_fade_pos++;
            } else if (!voice->preparing &&
                       voice->current_pos >= voice->snapshot_length) {
                voice->playing = false;
                voice->current_pos = 0;
            }
        }

        if (m->fading_out) {
            m->fade_gain -= m->fade_step;
            if (m->fade_gain < 0.0f) m->fade_gain = 0.0f;
            gain = master * m->fade_gain;
        }

        float mixed_l = left * gain;
        float mixed_r = right * gain;
        float frame_peak = fmaxf(fabsf(mixed_l), fabsf(mixed_r));
        float limiter_target =
            frame_peak > TOY_AUDIO_CLIP_THRESHOLD
                ? TOY_AUDIO_CLIP_THRESHOLD / frame_peak
                : 1.0f;
        if (limiter_target < m->limiter_gain) {
            m->limiter_gain = limiter_target;
        } else {
            m->limiter_gain +=
                (1.0f - m->limiter_gain) * limiter_release;
            if (m->limiter_gain > limiter_target) {
                m->limiter_gain = limiter_target;
            }
        }
        mixed_l *= m->limiter_gain;
        mixed_r *= m->limiter_gain;
        out[2 * i]     = mixed_l;
        out[2 * i + 1] = mixed_r;
        float abs_l = fabsf(mixed_l);
        float abs_r = fabsf(mixed_r);
        if (abs_l > peak) peak = abs_l;
        if (abs_r > peak) peak = abs_r;
    }

    m->quiet_recent_peak = peak;
    if (peak <= TOY_AUDIO_QUIET_THRESHOLD) {
        m->quiet_seconds += (float)nframes / TA_RATE;
        if (m->quiet_seconds > TOY_AUDIO_QUIET_SECONDS) {
            m->quiet_seconds = TOY_AUDIO_QUIET_SECONDS;
        }
    } else {
        m->quiet_seconds = 0.0f;
    }
}

void toy_mixer_start_voice(ToyMixer *m, const ToySamplePair *pair,
                           float volume_scale) {
    toy_mixer_start_voice_panned(m, pair, volume_scale, 1.0f, 1.0f);
}

bool toy_mixer_claim_voice(ToyMixer *m, int sample_length,
                           ToyVoiceClaim *claim) {
    if (!m || !claim || sample_length <= 0) return false;
    claim->voice_index = -1;
    claim->generation = 0;
    int voice_index = -1;
    int oldest_voice = -1;
    int max_pos = -1;

    for (int i = 0; i < TOY_AUDIO_MAX_VOICES; ++i) {
        ToyVoice *voice = &m->voices[i];
        if (!voice->playing && !voice->preparing &&
            voice->snapshot_capacity >= sample_length) {
            voice_index = i;
            break;
        }
        if (voice->playing && voice->snapshot_capacity >= sample_length &&
            voice->current_pos > max_pos) {
            max_pos = voice->current_pos;
            oldest_voice = i;
        }
    }

    if (voice_index == -1) {
        if (oldest_voice < 0) return false;
        voice_index = oldest_voice;
    }

    ToyVoice *voice = &m->voices[voice_index];

    if (voice->playing && voice->snapshot_data1 && voice->snapshot_data2) {
        int remaining = voice->snapshot_length - voice->current_pos;
        if (remaining < 0) remaining = 0;
        for (int i = 0; i < TOY_AUDIO_STEAL_CROSSFADE; ++i) {
            int pos = voice->current_pos + i;
            if (i < remaining) {
                float c = (voice->snapshot_data1[pos] + voice->snapshot_data2[pos]) *
                          0.5f * voice->volume_scale;
                voice->steal_tail_l[i] = c * voice->l_gain;
                voice->steal_tail_r[i] = c * voice->r_gain;
            } else {
                voice->steal_tail_l[i] = 0.0f;
                voice->steal_tail_r[i] = 0.0f;
            }
        }
        voice->steal_fade_pos = 0;
    } else {
        voice->steal_fade_pos = TOY_AUDIO_STEAL_CROSSFADE;
    }

    voice->playing = false;
    voice->preparing = true;
    voice->generation++;
    if (voice->generation == 0) voice->generation = 1;
    claim->voice_index = voice_index;
    claim->generation = voice->generation;
    return true;
}

static ToyVoice *claimed_voice(ToyMixer *m, ToyVoiceClaim claim) {
    if (!m || claim.voice_index < 0 ||
        claim.voice_index >= TOY_AUDIO_MAX_VOICES) {
        return NULL;
    }
    ToyVoice *voice = &m->voices[claim.voice_index];
    if (!voice->preparing || voice->generation != claim.generation) {
        return NULL;
    }
    return voice;
}

bool toy_mixer_copy_claimed_voice(ToyMixer *m, ToyVoiceClaim claim,
                                  const ToySamplePair *pair) {
    if (!pair || !pair->data1 || !pair->data2 || pair->length <= 0) {
        return false;
    }
    ToyVoice *voice = claimed_voice(m, claim);
    if (!voice || voice->snapshot_capacity < pair->length) return false;
    memcpy(voice->snapshot_data1, pair->data1,
           (size_t)pair->length * sizeof(float));
    memcpy(voice->snapshot_data2, pair->data2,
           (size_t)pair->length * sizeof(float));
    return true;
}

bool toy_mixer_commit_voice(ToyMixer *m, ToyVoiceClaim claim,
                            int sample_length, float volume_scale,
                            float l_gain, float r_gain) {
    ToyVoice *voice = claimed_voice(m, claim);
    if (!voice || sample_length <= 0 ||
        sample_length > voice->snapshot_capacity) {
        return false;
    }
    voice->snapshot_length = sample_length;
    voice->current_pos = 0;
    voice->volume_scale = volume_scale;
    voice->l_gain = l_gain;
    voice->r_gain = r_gain;
    voice->preparing = false;
    voice->playing = true;
    m->quiet_seconds = 0.0f;
    return true;
}

void toy_mixer_cancel_voice(ToyMixer *m, ToyVoiceClaim claim) {
    ToyVoice *voice = claimed_voice(m, claim);
    if (!voice) return;
    voice->preparing = false;
    voice->playing = false;
    voice->current_pos = 0;
}

void toy_mixer_start_voice_panned(ToyMixer *m, const ToySamplePair *pair,
                                  float volume_scale,
                                  float l_gain, float r_gain) {
    if (!m || !pair) return;
    ToyVoiceClaim claim;
    if (!toy_mixer_claim_voice(m, pair->length, &claim)) return;
    if (!toy_mixer_copy_claimed_voice(m, claim, pair) ||
        !toy_mixer_commit_voice(m, claim, pair->length, volume_scale,
                                l_gain, r_gain)) {
        toy_mixer_cancel_voice(m, claim);
    }
}

bool toy_mixer_any_voice_playing(const ToyMixer *m) {
    if (!m) return false;
    for (int v = 0; v < TOY_AUDIO_MAX_VOICES; ++v) {
        if (m->voices[v].playing) {
            return true;
        }
    }
    return false;
}

void toy_mixer_adjust_volume(ToyMixer *m, float delta) {
    if (!m || delta == 0.0f) return;

    float new_volume = m->master_volume + delta;
    if (new_volume < TOY_AUDIO_VOLUME_MIN) {
        new_volume = TOY_AUDIO_VOLUME_MIN;
    } else if (new_volume > TOY_AUDIO_VOLUME_MAX) {
        new_volume = TOY_AUDIO_VOLUME_MAX;
    }

    m->master_volume = new_volume;
    if (new_volume > TOY_AUDIO_VOLUME_MIN) {
        m->master_volume_before_mute = new_volume;
        m->muted = false;
    } else {
        m->master_volume = 0.0f;
        m->muted = true;
    }
}

void toy_mixer_set_mute(ToyMixer *m, bool mute) {
    if (!m || m->muted == mute) return;
    if (mute) {
        if (m->master_volume > TOY_AUDIO_VOLUME_MIN) {
            m->master_volume_before_mute = m->master_volume;
        }
        m->muted = true;
    } else {
        m->muted = false;
        if (m->master_volume <= TOY_AUDIO_VOLUME_MIN) {
            m->master_volume = fmaxf(m->master_volume_before_mute,
                                     TOY_AUDIO_VOLUME_STEP);
            m->master_volume = fminf(m->master_volume, TOY_AUDIO_VOLUME_MAX);
        }
    }
}

void toy_mixer_begin_fade_out(ToyMixer *m, float seconds) {
    if (!m) return;
    if (seconds <= 0.0f) {
        m->fade_gain = 0.0f;
        m->fade_step = 0.0f;
        m->fading_out = true;
        return;
    }
    m->fade_step = m->fade_gain / (seconds * TA_RATE);
    m->fading_out = true;
}

bool toy_mixer_is_quiet(const ToyMixer *m) {
    return m && m->quiet_seconds >= TOY_AUDIO_QUIET_SECONDS;
}

void toy_mixer_reset(ToyMixer *m) {
    if (!m) return;
    float volume = TOY_AUDIO_DEFAULT_VOLUME;
    for (int v = 0; v < TOY_AUDIO_MAX_VOICES; ++v) {
        free(m->voices[v].snapshot_data1);
        free(m->voices[v].snapshot_data2);
    }
    toy_mixer_init(m, volume);
}

bool toy_audio_reserve_scratch(int max_samples) {
    if (max_samples <= 0) return false;
    if (g_nostalgia_dry_capacity >= max_samples) return true;
    float *dry = realloc(g_nostalgia_dry,
                         (size_t)max_samples * sizeof(float));
    if (!dry) return false;
    g_nostalgia_dry = dry;
    g_nostalgia_dry_capacity = max_samples;
    return true;
}

void toy_audio_release_scratch(void) {
    free(g_nostalgia_dry);
    g_nostalgia_dry = NULL;
    g_nostalgia_dry_capacity = 0;
}


bool toy_sample_pair_alloc(ToySamplePair *pair, int length) {
    if (!pair || length <= 0) return false;
    pair->data1 = calloc((size_t)length, sizeof(float));
    pair->data2 = calloc((size_t)length, sizeof(float));
    if (!pair->data1 || !pair->data2) {
        toy_sample_pair_free(pair);
        return false;
    }
    pair->length = length;
    pair->dirty = true;
    return true;
}

void toy_sample_pair_free(ToySamplePair *pair) {
    if (!pair) return;
    free(pair->data1);
    pair->data1 = NULL;
    free(pair->data2);
    pair->data2 = NULL;
    pair->length = 0;
    pair->dirty = true;
}


float toy_audio_find_peak(const float *buffer, int nsamples) {
    float peak = 0.0f;
    for (int i = 0; i < nsamples; ++i) {
        float mag = fabsf(buffer[i]);
        if (mag > peak) {
            peak = mag;
        }
    }
    return peak;
}

void toy_audio_normalize(float *buffer, int nsamples, float target_peak) {
    if (!buffer || nsamples <= 0) {
        return;
    }
    float peak = toy_audio_find_peak(buffer, nsamples);
    if (peak <= 0.0f) {
        return;
    }
    float scale = target_peak / peak;
    for (int i = 0; i < nsamples; ++i) {
        buffer[i] *= scale;
    }
}

void toy_audio_micro_reverb(float * restrict buffer, int nsamples,
                            float time_scale, float rt60, float wet_mix) {
    static const float comb_sec[4] = { 0.0023f, 0.0031f, 0.0041f, 0.0053f };
    float comb_z[4][512] = {{0}};
    float ap_z[128] = {0};
    int   comb_d[4];
    float comb_g[4];
    float comb_lp[4] = { 0, 0, 0, 0 };
    for (int c = 0; c < 4; c++) {
        float delay_s = comb_sec[c] * time_scale;
        comb_d[c] = (int)(delay_s * TA_RATE);
        if (comb_d[c] < 1) comb_d[c] = 1;
        if (comb_d[c] > 511) comb_d[c] = 511;
        comb_g[c] = powf(10.0f, -3.0f * delay_s / rt60);
    }
    int ap_d = (int)(0.0011f * time_scale * TA_RATE);
    if (ap_d < 1) ap_d = 1;
    if (ap_d > 127) ap_d = 127;

    int idx_comb[4] = {0, 0, 0, 0};
    int idx_ap = 0;

    for (int i = 0; i < nsamples; i++) {
        float x = buffer[i];
        float wet = 0.0f;
        for (int c = 0; c < 4; c++) {
            float y = comb_z[c][idx_comb[c]];
            comb_lp[c] += 0.35f * (y - comb_lp[c]);  
            comb_z[c][idx_comb[c]] = x + comb_lp[c] * comb_g[c];
            idx_comb[c]++;
            if (idx_comb[c] >= comb_d[c]) {
                idx_comb[c] = 0;
            }
            wet += y;
        }
        wet *= 0.25f;
        float v = ap_z[idx_ap];
        float in = wet + v * 0.7f;
        ap_z[idx_ap] = in;
        idx_ap++;
        if (idx_ap >= ap_d) {
            idx_ap = 0;
        }
        wet = v - in * 0.7f;
        buffer[i] = x + wet_mix * wet;
    }
}

__attribute__((hot))
void toy_audio_generate_bounce(float * restrict buffer, int nsamples,
                               int pitch, int volume, float decay_time,
                               float noise_amount, float resonance_amount,
                               ToyBounceStyle style) {
    float resonant_freq = 35.0f * powf(2.0f, pitch / 12.0f);

    float amp = (volume / 64.0f) * 1.4f;

    float attack_time = TA_BOUNCE_ATTACK;

    float filter1 = 0.0f;
    float filter2 = 0.0f;
    float filter3 = 0.0f;

    const float SILENCE_THRESHOLD = 0.001f;
    bool nostalgia = (style == TOY_BOUNCE_NOSTALGIA);

    #define NOST_MAX_PARTIALS 40
    float nost_rot_c[NOST_MAX_PARTIALS];
    float nost_rot_s[NOST_MAX_PARTIALS];
    float nost_re[NOST_MAX_PARTIALS];
    float nost_im[NOST_MAX_PARTIALS];
    float nost_gain[NOST_MAX_PARTIALS];
    float nost_fade[NOST_MAX_PARTIALS];  
    float nost_amp[NOST_MAX_PARTIALS];
    unsigned char nost_deep[NOST_MAX_PARTIALS];  
    int   nost_n = 0;
    uint32_t nost_lcg = 0x0B01A5CDu;     
    if (nostalgia) {
        amp *= 0.82f;
        attack_time = 0.0015f;  

        float weight = 42.0f * powf(resonant_freq / 74.0f, 0.25f);
        if (weight < 42.0f)  weight = 42.0f;
        if (weight > 130.0f) weight = 130.0f;
        float sub = weight * 0.38f;
        const struct { float f, g, fade; } chord[] = {
            { sub,                 0.30f, 0.0f },  
            { sub + 0.75f,         0.22f, 0.0f },  
            { weight * 0.76f,      0.42f, 0.6f },  
            { weight,              1.00f, 0.5f },  
            { weight + 2.7f,       0.85f, 0.5f },  
            { weight * 1.27f,      0.48f, 0.9f },  
            { weight * 1.27f + 3.4f, 0.34f, 0.9f },
            { weight * 1.95f,      0.36f, 1.4f },  
            { weight * 2.90f,      0.32f, 2.0f },  
            { weight * 3.80f,      0.24f, 2.6f },  
        };
        int n_chord = (int)(sizeof(chord) / sizeof(chord[0]));
        for (int k = 0; k < n_chord; k++) {
            nost_deep[nost_n] = chord[k].f < weight * 1.15f;
            float w = 2.0f * TA_PI * chord[k].f / TA_RATE;
            nost_rot_c[nost_n] = cosf(w);
            nost_rot_s[nost_n] = sinf(w);
            nost_re[nost_n]   = 1.0f;   
            nost_im[nost_n]   = 0.0f;
            nost_gain[nost_n] = chord[k].g;
            nost_fade[nost_n] = expf(-chord[k].fade /
                                     (decay_time * TA_RATE));
            nost_amp[nost_n]  = 1.0f;
            nost_n++;
        }
        float f = weight * 0.65f;
        float f_top = weight * 9.0f;
        while (nost_n < NOST_MAX_PARTIALS && f < f_top) {
            nost_lcg = nost_lcg * 1664525u + 1013904223u;
            float u1 = (float)(nost_lcg >> 8) * (1.0f / 16777216.0f);
            nost_lcg = nost_lcg * 1664525u + 1013904223u;
            float u2 = (float)(nost_lcg >> 8) * (1.0f / 16777216.0f);
            nost_lcg = nost_lcg * 1664525u + 1013904223u;
            float u3 = (float)(nost_lcg >> 8) * (1.0f / 16777216.0f);
            f += 6.0f + u1 * 46.0f;      
            if (f >= f_top) break;
            nost_deep[nost_n] = f < weight * 1.15f;
            float w = 2.0f * TA_PI * f / TA_RATE;
            float phase = u3 * 2.0f * TA_PI;
            nost_rot_c[nost_n] = cosf(w);
            nost_rot_s[nost_n] = sinf(w);
            nost_re[nost_n]   = cosf(phase);
            nost_im[nost_n]   = sinf(phase);
            nost_gain[nost_n] = 0.60f * powf(weight / f, 0.75f) *
                                (0.55f + 0.9f * u2);
            nost_fade[nost_n] = expf(-(0.5f + 2.4f * (f / f_top)) /
                                     (decay_time * TA_RATE));
            nost_amp[nost_n]  = 1.0f;
            nost_n++;
        }
    }

    for (int i = 0; i < nsamples; i++) {
        float t = i / TA_RATE;

        float env;
        if (t < attack_time) {
            env = t / attack_time;
        } else {
            env = expf(-(t - attack_time) / decay_time);
        }

        if (t > attack_time && env < SILENCE_THRESHOLD) {
            memset(buffer + i, 0, (nsamples - i) * sizeof(float));
            break;
        }

        float noise = (float)(((double)rand() / (double)RAND_MAX) * 2.0 - 1.0);
        float mixed;

        if (!nostalgia) {
            float cutoff = 0.15f;
            filter1 = filter1 + cutoff * (noise - filter1) + 1e-18f;
            filter2 = filter2 + cutoff * (filter1 - filter2) + 1e-18f;
            filter3 = filter3 + cutoff * (filter2 - filter3) + 1e-18f;

            float resonance = sinf(2.0f * TA_PI * resonant_freq * t) * 0.15f;

            mixed = filter3 * noise_amount + resonance * resonance_amount;

            float total_amount = noise_amount + resonance_amount;
            if (total_amount > 0.0f) {
                mixed *= 1.0f / total_amount;
            }
        } else {
            (void)resonance_amount;
            float chord_deep = 0.0f;
            float chord_high = 0.0f;
            for (int k = 0; k < nost_n; k++) {
                float v = nost_gain[k] * nost_amp[k] * nost_im[k];
                if (nost_deep[k]) chord_deep += v; else chord_high += v;
                float re = nost_re[k];
                nost_re[k] = re * nost_rot_c[k] - nost_im[k] * nost_rot_s[k];
                nost_im[k] = re * nost_rot_s[k] + nost_im[k] * nost_rot_c[k];
                nost_amp[k] *= nost_fade[k];
            }

            float splash_env = expf(-t / 0.016f);
            float splash_src = noise * splash_env;
            filter1 = filter1 + 0.22f * (splash_src - filter1) + 1e-18f;
            filter2 = filter2 + 0.13f * (filter1 - filter2) + 1e-18f;
            float splash = filter2 * (0.9f + 0.6f * noise_amount);

            nost_lcg = nost_lcg * 1664525u + 1013904223u;
            float cr_u = (float)(nost_lcg >> 8) * (1.0f / 16777216.0f);
            float crackle = 0.0f;
            float cr_density = 0.0003f + 0.010f * env * env;
            if (cr_u < cr_density) {
                nost_lcg = nost_lcg * 1664525u + 1013904223u;
                float cr_a = (float)(nost_lcg >> 8) * (1.0f / 8388608.0f)
                             - 1.0f;                    
                cr_a = cr_a * cr_a * cr_a;              
                crackle = cr_a * (0.18f + 0.85f * sqrtf(env));
            }

            float growl = tanhf(chord_deep * 1.9f) * 0.6456f;  
            mixed = growl * 0.80f + chord_high * 0.72f + splash;
            mixed = 0.55f * tanhf(mixed * 1.818f);  
            mixed += crackle;
        }

        float sample = mixed * amp * env;
        buffer[i] = sample;
    }

    if (nostalgia) {
        if (g_nostalgia_dry && nsamples <= g_nostalgia_dry_capacity) {
            float *dry = g_nostalgia_dry;
            memcpy(dry, buffer, (size_t)nsamples * sizeof(float));
            float rt60 = 4.2f + 3.0f * decay_time;
            static const float comb_sec[4] = { 0.0437f, 0.0511f,
                                               0.0617f, 0.0713f };
            static float comb_z[4][3600];
            static float ap1_z[280];
            static float ap2_z[112];
            memset(comb_z, 0, sizeof(comb_z));
            memset(ap1_z, 0, sizeof(ap1_z));
            memset(ap2_z, 0, sizeof(ap2_z));
            int   comb_d[4];
            float comb_g[4];
            float comb_lp[4] = { 0, 0, 0, 0 };
            for (int c = 0; c < 4; c++) {
                comb_d[c] = (int)(comb_sec[c] * TA_RATE);
                comb_g[c] = powf(10.0f, -3.0f * comb_sec[c] / rt60);
            }
            int ap1_d = (int)(0.0053f * TA_RATE);
            int ap2_d = (int)(0.0017f * TA_RATE);
            int predelay = (int)(0.030f * TA_RATE);
            int er1 = (int)(0.055f * TA_RATE);
            int er2 = (int)(0.113f * TA_RATE);
            float er_lp = 0.0f;
            const float wet_mix = 0.90f;  
            for (int i = 0; i < nsamples; i++) {
                float x = (i >= predelay) ? dry[i - predelay] : 0.0f;
                float wet = 0.0f;
                for (int c = 0; c < 4; c++) {
                    int zi = i % comb_d[c];
                    float y = comb_z[c][zi];
                    comb_lp[c] += 0.35f * (y - comb_lp[c]);  
                    comb_z[c][zi] = x + comb_lp[c] * comb_g[c];
                    wet += y;
                }
                wet *= 0.25f;
                int i1 = i % ap1_d;
                float v1 = ap1_z[i1];
                float in1 = wet + v1 * 0.7f;
                ap1_z[i1] = in1;
                wet = v1 - in1 * 0.7f;
                int i2 = i % ap2_d;
                float v2 = ap2_z[i2];
                float in2 = wet + v2 * 0.7f;
                ap2_z[i2] = in2;
                wet = v2 - in2 * 0.7f;
                float er = 0.0f;
                if (i >= er1) er += dry[i - er1] * 0.20f;
                if (i >= er2) er += dry[i - er2] * 0.12f;
                er_lp += 0.20f * (er - er_lp);
                float out = dry[i] + er_lp + wet_mix * wet;
                buffer[i] = floorf(out * 128.0f + 0.5f) * (1.0f / 128.0f);
            }
        }
    }
}

int toy_audio_bounce_pair_length(void) {
    return (int)((TA_BOUNCE_ATTACK +
                  TA_BOUNCE_MAX_DECAY * TA_BOUNCE_TAIL_MULTIPLIER) * TA_RATE) +
           TA_BOUNCE_BUFFER_SLACK;
}

void toy_audio_generate_bounce_pair(ToySamplePair *pair, float size_scale,
                                    ToyBounceStyle style) {
    if (!pair || !pair->data1 || !pair->data2 || pair->length <= 0) {
        return;
    }
    size_scale = fmaxf(size_scale, 0.05f);
    float clamped_for_normalization = fminf(size_scale, 1.5f);

    float octave_shift = log2f(1.0f / clamped_for_normalization);
    int pitch_offset = (int)roundf(fmaxf(0.0f, octave_shift * 12.0f));
    if (pitch_offset > 36) pitch_offset = 36;  

    float decay = TA_BOUNCE_MAX_DECAY * powf(size_scale, 1.2f);
    decay = fminf(TA_BOUNCE_MAX_DECAY, fmaxf(0.07f, decay));

    float noise_amount = 0.9f * powf(size_scale, 1.4f);
    noise_amount = fmaxf(0.05f, fminf(0.9f, noise_amount));

    float resonance_amount = 0.18f +
        powf(fmaxf(0.0f, 1.0f - clamped_for_normalization), 0.75f) * 0.85f;
    resonance_amount = fminf(0.95f, resonance_amount);

    toy_audio_generate_bounce(pair->data1, pair->length, 13 + pitch_offset,
                              32, decay, noise_amount, resonance_amount, style);
    toy_audio_normalize(pair->data1, pair->length, TOY_AUDIO_NORMALIZE_TARGET);
    toy_audio_generate_bounce(pair->data2, pair->length, 11 + pitch_offset,
                              32, decay, noise_amount, resonance_amount, style);
    toy_audio_normalize(pair->data2, pair->length, TOY_AUDIO_NORMALIZE_TARGET);
    pair->dirty = false;
}

void toy_audio_equal_power_pan(float pan, float *l_gain, float *r_gain) {
    if (pan < 0.0f) pan = 0.0f;
    if (pan > 1.0f) pan = 1.0f;
    float theta = pan * TA_PI * 0.5f;
    if (l_gain) *l_gain = cosf(theta);
    if (r_gain) *r_gain = sinf(theta);
}


void toy_hiss_init(ToyHiss *h) {
    h->noise = 0x5eedbeefU;   
    h->env = 0.0f;
    h->burst = 0.0f;
    h->lowpass = 0.0f;
    h->lfo_phase = 0.0f;
    h->prev_active = false;
}

bool toy_hiss_is_idle(const ToyHiss *h) {
    return h->env < TOY_HISS_IDLE_ENV;
}

void toy_hiss_render(ToyHiss *h, float *out, int nsamples,
                     bool active, float size01, int sample_rate) {
    if (sample_rate <= 0) sample_rate = TOY_AUDIO_SAMPLE_RATE;
    const float rate = (float)sample_rate;

    const float atk = 1.0f - expf(-1.0f / (0.020f * rate));
    const float rel = 1.0f - expf(-1.0f / (0.080f * rate));
    const float bdec = 1.0f - expf(-1.0f / (0.055f * rate));
    const float lfo_step = 2.0f * TA_PI * 5.3f / rate;

    if (active && !h->prev_active) h->burst = 1.0f;   
    h->prev_active = active;

    const float target = active ? 1.0f : 0.0f;
    const float lowmix = 0.10f + 0.30f * size01;   
    const float vol = 0.15f + 0.11f * size01;      

    for (int i = 0; i < nsamples; i++) {
        h->noise ^= h->noise << 13;
        h->noise ^= h->noise >> 17;
        h->noise ^= h->noise << 5;
        float n = (float)(int32_t)h->noise * (1.0f / 2147483648.0f);

        h->lowpass += 0.16f * (n - h->lowpass);
        float s = (n - h->lowpass) + h->lowpass * lowmix * 3.0f;

        h->env += (target - h->env) * (target > h->env ? atk : rel);
        h->burst -= h->burst * bdec;

        h->lfo_phase += lfo_step;
        if (h->lfo_phase > 2.0f * TA_PI) h->lfo_phase -= 2.0f * TA_PI;
        float wob = 1.0f + 0.05f * sinf(h->lfo_phase);

        float value = s * vol * h->env * (1.0f + 0.9f * h->burst) * wob;
        if (value > 1.0f) value = 1.0f;
        if (value < -1.0f) value = -1.0f;
        out[i] = value;
    }
}
