/*
 * bsound: sound for a game on the badge (docs/SOUND.md), as SDL's audio
 * callback gives it: a thread asks the game for the next stretch of samples
 * and plays it, on the speaker or to the AirPlay speaker the Sound panel
 * chose. The thread opens the device again when that choice changes, and
 * plays nothing while there is no output.
 *
 * Samples are signed 16-bit, interleaved when there are two channels. A
 * period is about 10 ms; four of them are queued, so a sound starts about
 * 40 ms after the game makes it (plus AirPlay's delay there).
 *
 * BSOUND_FILE=path in the environment writes what would play into that file
 * instead (16-bit little-endian stereo at the rate asked for), in real time.
 */
#ifndef BSOUND_H
#define BSOUND_H

#include <stdint.h>

/* the next frames: count frames of channels samples each, into out */
typedef void (*bsound_fill)(void *user, int16_t *out, unsigned int frames);

/* Start playing at rate (8000 to 48000) with 1 or 2 channels: 0, or -1. */
int bsound_start(unsigned int rate, unsigned int channels, bsound_fill fill, void *user);

/* The frames a call of fill asks for. */
unsigned int bsound_period(void);

/* fill is not called between these (SDL_LockAudio) */
void bsound_lock(void);
void bsound_unlock(void);

/* 1: silence instead of fill, 0: fill again; may come before bsound_start */
void bsound_pause(int on);

/* Stop the thread and close the device. */
void bsound_stop(void);

#endif
