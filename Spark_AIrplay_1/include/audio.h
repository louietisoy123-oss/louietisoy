#ifndef AIRPLAY_AUDIO_H_
#define AIRPLAY_AUDIO_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool audio_producer_acquire(const char *name);
void audio_producer_release(void);
size_t audio_play_pcm(const int16_t *pcm, size_t frames);
void audio_airplay_set_volume(float db);
void audio_diag_tone_stop(void);
void audio_diag_tone_start(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRPLAY_AUDIO_H_ */
