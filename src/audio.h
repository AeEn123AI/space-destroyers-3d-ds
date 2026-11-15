/* Audio API used by game.c (DS implementation in audio_ds.c). */
#ifndef AUDIO_H
#define AUDIO_H
#include <stdint.h>

enum { SND_POP, SND_POP_STAGE, SND_CRUNCH, SND_HIT, SND_LOSE, SND_EASTER, SND_COUNT };
enum { GRP_STAGE, GRP_THUMB, GRP_SPRITE, GRP_COUNT };


int  audio_init(void);                 /* 0: embedded music failed its checksum (sound effects still work) */
void audio_update(void);               /* once per frame */
void audio_suspend(void);              /* around sleep mode */
void audio_resume(void);

/* game-side API (thread safe) */
void audio_play(int snd, int group);
void audio_stop_all(void);
void audio_music_start(int with_intro);
int  audio_music_playing(void);
int  audio_sound_playing(int snd);
void audio_set_group_volume(int group, float vol);   /* 0..1 */

#endif
