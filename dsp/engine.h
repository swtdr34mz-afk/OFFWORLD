/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (OFFWORLD) */
/* OFFWORLD's sound engine: one instrument, an 80s brass synth for drones.
 *
 *   voice     two saws (or saw and pulse) a little apart, a sine sub an octave down, a 24 dB low-pass with its own
 *             brass envelope (it opens slowly, like a horn section swelling), an amp envelope, a delayed vibrato,
 *             and a slow random drift of its pitch (an old analogue's)
 *   effects   an ensemble chorus (the 80s width), a dark ping-pong delay, a reverb of eight lines (up to 30 s, FREEZE)
 *   rain      hiss, drops, the city's rumble, and thunder far off (at STORM and FLOOD)
 *
 * Every preset is a TIMBRE (the oscillators, the filter) plus four MACROS, one per knob, 0..127. A macro moves several
 * parameters at once, each along its own curve, and has SWEETSPOT ZONES: positions where everything it moves lands
 * on a value that sounds right together (their names are shown). The app makes a knob rest on a zone for a few
 * detents; the engine turns the four values into the sound.
 *
 * The main loop talks to the engine through eng_* calls (notes go through a small queue); eng_render runs in the
 * audio interrupt. */
#pragma once
#include <stdint.h>

#define ENG_SR 44100.0f
#define ENG_NV 8                         /* voices: notes at once */

/* a macro: its name and its sweetspot zones (centre 0..127, name), left to right */
#define MACRO_ZONES 5
typedef struct {
    const char *name;
    uint8_t nzones;
    uint8_t centre[MACRO_ZONES];
    const char *zone[MACRO_ZONES];
} macro_t;
extern const macro_t MACROS[4];          /* K1 SWELL, K2 DRIFT, K3 SPACE, K4 RAIN */

/* a preset: its name, its timbre, and the macros it starts at */
typedef struct {
    const char *name;
    float o2_semi;                       /* the second oscillator, semitones from the first (0, 7, 12, -12 ...) */
    uint8_t o2_pulse;                    /* 1: the second is a pulse (hollow, reedy) */
    float pw;                            /* its width 0.5 .. 0.9 */
    float o2_lvl, sub;                   /* the second's level, the sub's */
    float cut_hz;                        /* the filter closed */
    float env_oct;                       /* how far the brass envelope opens it, in octaves */
    float res;                           /* resonance 0 .. 0.9 */
    float bright;                        /* velocity's share in the opening, 0 .. 1 */
    uint8_t m[4];                        /* the macros it starts at */
} preset_t;
#define NPRESETS 8
extern const preset_t PRESETS[NPRESETS];

void eng_init(void);
/* the sound: the preset's timbre, the four macros, TONE -1..1 (the filter darker / brighter), VIBRATO 0..1 */
void eng_sound(int preset, const uint8_t m[4], float tone, float vib);
/* a note: id names it (the same id again strikes it again), pitch in MIDI semitones, velocity 0..1 */
void eng_note_on(int id, float pitch, float vel);
void eng_note_off(int id);
void eng_all_off(void);                  /* every note let go (they ring out) */
void eng_panic(void);                    /* silence now */
void eng_shed(void);                     /* the render ran late: a voice let go quickly (audio interrupt) */
void eng_render(int32_t *lr, uint32_t n, float gain);

/* for the screen and the tests */
int eng_voices(void);                    /* voices sounding */
float eng_level(void);                   /* the output, 0..1, smoothed */
uint32_t eng_thunder(void);              /* thunders since the start (the screen flashes) */
uint32_t eng_notes(void);                /* notes started since the start */
