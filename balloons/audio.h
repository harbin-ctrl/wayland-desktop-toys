#ifndef BALLOONS_AUDIO_H
#define BALLOONS_AUDIO_H

#include <stdbool.h>

typedef enum {
    BOUNCE_SOUND_POINGO = 0,    
    BOUNCE_SOUND_NOSTALGIA = 1  
} BounceSoundStyle;

bool audio_init(void);

void audio_shutdown(void);

void audio_shutdown_graceful(void);

void play_bounce_sound(int volume);  
void play_pop_sound(int volume);     
void play_thump_sound(int volume);   

void play_thunder_sound(float distance01);

bool play_whoosh_async(float strength01, float duration_seconds, float gain,
                       bool is_gust);

bool whoosh_gust_poll(float *delay_seconds);

void whoosh_gust_cancel(void);

void whoosh_stop_playing(void);

void audio_pregen_async(void);

void mark_sounds_dirty(float size_scale);

void adjust_master_volume(float delta);
void set_master_mute(bool mute);
void toggle_master_mute(void);
bool audio_is_muted(void);

void set_bounce_sound_style(BounceSoundStyle style);

#endif /* BALLOONS_AUDIO_H */
