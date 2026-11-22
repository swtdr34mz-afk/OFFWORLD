/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (OFFWORLD) */
/* OFFWORLD: input, the knobs and their sweetspots, DRONE, EPIC, the low split and the melody that follows it,
 * pressure, the pages, settings. Part of the unity build (device/unity.c, host/plat_host.c), after the platform:
 * gfx.c and announce.c are in scope. The sound is dsp/engine.c, the film on the screen city.inc.
 *
 * Controls (v0.2)
 *   the keys        the brass, with velocity (harder = brighter) and pressure (PRESSURE); OCT- / OCT+ the octave
 *   K1..K4          MAIN: SWELL, DRIFT, SPACE, RAIN, macros resting on sweetspots
 *                   SPLIT (SEL): SPLIT (off, 1..3 octaves down), POINT, SCALE, FOLLOW (OFF, SNAP, WHITE)
 *                   PRESSURE (GLO): PRESSURE (OFF, FILTER, SWELL, POWER, VIBRATO), AMOUNT, TONE, VIBRATO
 *   SEL             the SPLIT page (again: MAIN); held + a key: the split there
 *   GLO             the PRESSURE page (again: MAIN)       HOME  MAIN; held 1 s: the preset's knobs as they came
 *   PRESET          the preset (each keeps its own knobs)
 *   SELECT          TONE              ALGORITHM  VIBRATO
 *   PLAY            DRONE: every note stays (a key with no key down on its side starts a new chord), a deep root
 *   SEQ             LOW HOLD: the low split's notes stay (a new low note replaces them); the right hand plays free
 *   ARP             EPIC: root, fifth and octave from one finger
 *   REC held 1 s    every note let go
 * The melody: with the split on and FOLLOW on, the right hand plays in SCALE from the low split's note (the lowest
 * that sounds, or the last one): SNAP moves every key to the scale's note at or below it, WHITE puts the scale on the
 * white keys (the black keys rest). */
#include "fastmath.h"
#include "engine.h"

#define SET_MAGIC 0x31574F46u            /* "FOW1": the settings record; another magic loads the defaults */
typedef struct {
    uint32_t magic;
    uint8_t m[NPRESETS][4];              /* every preset's knobs */
    uint8_t preset;
    int8_t tone;                         /* -50 .. 50 */
    uint8_t vib;                         /* 0 .. 100 */
    int8_t octave;                       /* -2 .. 1 */
    uint8_t epic;
    uint8_t split_oct;                   /* 0 off, 1..3 octaves down (v0.2; 0.1's records have zeros here) */
    uint8_t split_key;                   /* the right part's first key 1..26 (0: a 0.1 record: the defaults) */
    uint8_t scale;                       /* SCALES */
    uint8_t follow;                      /* FOLLOW_* */
    uint8_t press;                       /* PRESS_* */
    uint8_t pamt;                        /* 0 .. 100 */
    uint8_t rsv;
} settings_t;
static settings_t set;
static uint8_t set_dirty;
static uint32_t dirty_at;

#define DEEP_ID 999                      /* the drone's deep root */
#define MIDI_ID 300                      /* MIDI notes: 300 + note (+100, +200: EPIC's fifth and octave) */

#define C_CYAN RGB(60, 230, 255)
#define C_MAG RGB(255, 60, 170)
#define C_AMBER RGB(255, 170, 60)
#define C_FIRE RGB(255, 120, 30)
#define C_RAIN RGB(110, 130, 168)

/* ------------------------------------------------------------------ scales --- */
typedef struct {
    const char *name;
    uint8_t n;
    uint8_t st[7];
} scale_t;
static const scale_t SCALES[] = {
    {"MINOR", 7, {0, 2, 3, 5, 7, 8, 10}},
    {"DORIAN", 7, {0, 2, 3, 5, 7, 9, 10}},
    {"PHRYGIAN", 7, {0, 1, 3, 5, 7, 8, 10}},
    {"HARM MINOR", 7, {0, 2, 3, 5, 7, 8, 11}},
    {"MAJOR", 7, {0, 2, 4, 5, 7, 9, 11}},
    {"LYDIAN", 7, {0, 2, 4, 6, 7, 9, 11}},
    {"PENTATONIC", 5, {0, 3, 5, 7, 10}},
    {"ARABIC", 7, {0, 1, 4, 5, 7, 8, 11}},
};
#define NSCALE (sizeof SCALES / sizeof SCALES[0])
enum { FOLLOW_OFF, FOLLOW_SNAP, FOLLOW_WHITE, NFOLLOW };
static const char *const FOLLOW_NAME[NFOLLOW] = {"OFF", "SNAP", "WHITE"};
static const char *const PRESS_NAME[NPRESS] = {"OFF", "FILTER", "SWELL", "POWER", "VIBRATO"};
static const char *const NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

/* --------------------------------------------------------------- settings --- */
static void settings_v2(void)            /* the parts 0.2 added, as they start */
{
    set.split_oct = 0;
    set.split_key = 7;                   /* F3..B3 left, C4 the right's first */
    set.scale = 0;
    set.follow = FOLLOW_WHITE;
    set.press = PRESS_FILTER;
    set.pamt = 70;
    set.rsv = 0;
}

static void settings_defaults(void)
{
    uint32_t i, k;
    memset(&set, 0, sizeof set);
    set.magic = SET_MAGIC;
    for (i = 0; i < NPRESETS; i++)
        for (k = 0; k < 4u; k++)
            set.m[i][k] = PRESETS[i].m[k];
    set.vib = 30;
    settings_v2();
}

static int settings_ok(void)
{
    uint32_t i, k;
    for (i = 0; i < NPRESETS; i++)
        for (k = 0; k < 4u; k++)
            if (set.m[i][k] > 127u)
                return 0;
    return set.magic == SET_MAGIC && set.preset < NPRESETS && set.tone >= -50 && set.tone <= 50 && set.vib <= 100u &&
           set.octave >= -2 && set.octave <= 1 && set.epic <= 1u && set.split_oct <= 3u && set.split_key >= 1u &&
           set.split_key < NKEYS && set.scale < NSCALE && set.follow < NFOLLOW && set.press < NPRESS &&
           set.pamt <= 100u;
}

static void settings_load(void)
{
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set)
        settings_defaults();
    if (set.magic == SET_MAGIC && set.split_key == 0u)
        settings_v2();                   /* 0.1's record: its presets kept */
    if (!settings_ok())
        settings_defaults();
}

static void dirty(void)
{
    set_dirty = 1;
    dirty_at = plat_ms();
}

static void settings_save(void)
{
    if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0)
        set_dirty = 0;
}

#define KM (set.m[set.preset])           /* the picked preset's knobs */
#define PR (PRESETS[set.preset])

static void sound(void)
{
    eng_sound(set.preset, KM, (float)set.tone / 50.0f, (float)set.vib / 100.0f, set.press, (float)set.pamt / 100.0f);
}

/* ------------------------------------------------------- knobs: sweetspots --- */
#define REST 5
static int32_t knob_pos[4];

static int32_t pos_max(const macro_t *mc) { return 127 + mc->nzones * REST; }

static int32_t val_of_pos(const macro_t *mc, int32_t p)
{
    uint32_t z;
    for (z = 0; z < mc->nzones; z++) {
        int32_t c = mc->centre[z];
        if (p <= c)
            return p;
        if (p <= c + REST)
            return c;
        p -= REST;
    }
    return p > 127 ? 127 : p;
}

static int32_t pos_of_val(const macro_t *mc, int32_t v)
{
    int32_t p = v;
    uint32_t z;
    for (z = 0; z < mc->nzones; z++) {
        if (mc->centre[z] < v)
            p += REST;
        else if (mc->centre[z] == v)
            p += REST / 2;
    }
    return p;
}

static int zone_at(const macro_t *mc, int32_t v)
{
    uint32_t z;
    for (z = 0; z < mc->nzones; z++)
        if (mc->centre[z] == v)
            return (int)z;
    return -1;
}

static int zone_near(const macro_t *mc, int32_t v)
{
    uint32_t z;
    int best = 0;
    int32_t d = 999;
    for (z = 0; z < mc->nzones; z++) {
        int32_t e = v - mc->centre[z];
        if (e < 0)
            e = -e;
        if (e < d) {
            d = e;
            best = (int)z;
        }
    }
    return best;
}

static void knobs_from_settings(void)
{
    uint32_t k;
    for (k = 0; k < 4u; k++)
        knob_pos[k] = pos_of_val(&MACROS[k], KM[k]);
}

/* ---------------------------------------------------------------------- UI --- */
enum { PG_MAIN, PG_SPLIT, PG_PRESS, NPAGE };
enum { SIDE_L, SIDE_R };
static struct {
    char msg[2][24];
    uint32_t msg_until;
    int8_t zone_k, zone_z;
    uint32_t zone_until;
    uint32_t knob_at[4];
    uint32_t keys, btn;                  /* held last frame */
    uint32_t latched;                    /* keys whose notes stay (DRONE, LOW HOLD) */
    uint32_t sounding;                   /* keys whose notes sound now (held or latched) */
    float kp[NKEYS];                     /* the pitch each key started at */
    uint8_t prs[NKEYS];                  /* each key's pressure as last sent */
    uint8_t drone, lowhold, page;
    uint8_t root;                        /* the melody's root: the low split's note, 0..11 */
    float deep;                          /* the deep root's pitch (0: none) */
    uint32_t rec_t, home_t, sel_t;
    uint8_t sel_used;
    uint32_t enc_at[NE];
    int32_t enc_acc[4];
    uint8_t redraw, head_redraw;
    uint8_t intro;
    uint32_t intro_t0;
    uint32_t scene_at;
    uint8_t win[NKEYS];                  /* each key's light 0..255 (the film's lights follow them) */
    uint32_t rnd;
} ui;

static uint32_t urand(void)
{
    ui.rnd = ui.rnd * 1664525u + 1013904223u;
    return ui.rnd >> 8;
}

static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}

static void int_text(int32_t v, char *b)
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}

static void signed_text(int32_t v, char *b)
{
    if (v >= 0)
        *b++ = '+';
    int_text(v, b);
}

static void note_text(int32_t midi, char *b)   /* C4 = 60 */
{
    b[0] = 0;
    text_cat(b, NOTE_NAME[midi % 12]);
    int_text(midi / 12 - 1, b + (NOTE_NAME[midi % 12][1] ? 2 : 1));
}

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[48];
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1200u;
    ui.redraw = 1;
    if (ann_on()) {
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

static int32_t accel(int k, int32_t e, int32_t mid, int32_t fast)
{
    uint32_t now = plat_ms(), dt = now - ui.enc_at[k];
    ui.enc_at[k] = now;
    if (dt < 35u)
        return e * fast;
    if (dt < 80u)
        return e * mid;
    return e;
}

/* ------------------------------------------------------------------ notes --- */
static int split_on(void) { return set.split_oct != 0u; }
static int side_of(uint32_t k) { return split_on() && k < set.split_key ? SIDE_L : SIDE_R; }
static uint32_t side_mask(int side)
{
    uint32_t all = (1u << NKEYS) - 1u, left = split_on() ? (1u << set.split_key) - 1u : 0u;
    return side == SIDE_L ? left : all & ~left;
}
static int holds(int side) { return ui.drone || (side == SIDE_L && ui.lowhold && split_on()); }
static int is_white(uint32_t k)
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS;
    uint32_t i;
    for (i = 0; i < 16u; i++)
        if (WK[i] == k)
            return 1;
    return 0;
}

/* what key k plays now: 0 = nothing (a black key in WHITE) */
static int key_pitch(uint32_t k, float *p)
{
    float nat = 53.0f + (float)k + 12.0f * (float)set.octave;
    const scale_t *sc = &SCALES[set.scale % NSCALE];
    if (!split_on()) {
        *p = nat;
        return 1;
    }
    if (k < set.split_key) {             /* the low split: octaves down */
        *p = nat - 12.0f * (float)set.split_oct;
        return 1;
    }
    if (set.follow == FOLLOW_SNAP) {     /* to the scale's note at or below it */
        int32_t n = (int32_t)nat, rel = ((n - (int32_t)ui.root) % 12 + 12) % 12, best = 0, i;
        for (i = 0; i < sc->n; i++)
            if (sc->st[i] <= rel)
                best = sc->st[i];
        *p = (float)(n - rel + best);
        return 1;
    }
    if (set.follow == FOLLOW_WHITE) {    /* the scale on the white keys, from the root near the first one */
        uint32_t j, first = 0, d = 0;
        int32_t p0, base;
        if (!is_white(k))
            return 0;
        for (j = set.split_key; j < NKEYS; j++)
            if (is_white(j)) {
                first = j;
                break;
            }
        for (j = first; j < k; j++)
            d += (uint32_t)is_white(j);
        p0 = 53 + (int32_t)first + 12 * set.octave;
        base = p0 - (((p0 - (int32_t)ui.root) % 12 + 12) % 12);
        if (p0 - base > 6)
            base += 12;
        *p = (float)(base + 12 * (int32_t)(d / sc->n) + sc->st[d % sc->n]);
        return 1;
    }
    *p = nat;
    return 1;
}

static void key_start(uint32_t k, uint8_t vel)
{
    float p, v = (float)vel / 127.0f;
    if (!key_pitch(k, &p))
        return;
    ui.kp[k] = p;
    eng_note_on((int)k, p, v);
    if (set.epic) {
        eng_note_on(100 + (int)k, p + 7.0f, v * 0.85f);
        eng_note_on(200 + (int)k, p + 12.0f, v * 0.8f);
    }
    ui.sounding |= 1u << k;
}

static void key_stop(uint32_t k)
{
    eng_note_off((int)k);
    eng_note_off(100 + (int)k);
    eng_note_off(200 + (int)k);
    ui.sounding &= ~(1u << k);
    ui.prs[k] = 0;
}

/* the melody's root: the lowest note of the low split that sounds (else it stays the last) */
static void root_update(void)
{
    uint32_t k, left = side_mask(SIDE_L) & ui.sounding;
    if (!left)
        return;
    for (k = 0; k < NKEYS; k++)
        if (left >> k & 1u) {
            uint8_t r = (uint8_t)(((int32_t)ui.kp[k] % 12 + 12) % 12);
            if (r != ui.root) {
                ui.root = r;
                ui.redraw = 1;
            }
            return;
        }
}

/* the deep root under a drone: an octave below the lowest note that stays */
static void deep_update(void)
{
    float want = 0.0f;
    uint32_t k;
    if (ui.drone)
        for (k = 0; k < NKEYS; k++)
            if ((ui.latched & ui.sounding) >> k & 1u) {
                want = ui.kp[k] - 12.0f;
                break;
            }
    if (want == ui.deep)
        return;
    if (ui.deep > 0.0f)
        eng_note_off(DEEP_ID);
    if (want > 0.0f)
        eng_note_on(DEEP_ID, want, 0.65f);
    ui.deep = want;
}

static void latched_release(uint32_t mask)   /* the latched notes in mask let go */
{
    uint32_t k;
    for (k = 0; k < NKEYS; k++)
        if ((ui.latched & mask) >> k & 1u)
            key_stop(k);
    ui.latched &= ~mask;
}

static void key_press(uint32_t k, uint8_t vel, uint32_t down_before)
{
    int side = side_of(k);
    if (holds(side)) {
        if (!(down_before & side_mask(side)))   /* no key down on its side: a new chord there */
            latched_release(side_mask(side));
        ui.latched |= 1u << k;
    }
    key_start(k, vel);
    if (!(ui.sounding >> k & 1u))
        ui.latched &= ~(1u << k);        /* (a resting black key) */
    root_update();
    deep_update();
}

static void key_release(uint32_t k)
{
    if (!(holds(side_of(k)) && (ui.latched >> k & 1u)))
        key_stop(k);
    else if (ui.prs[k]) {                /* it stays, but no longer pressed */
        eng_press((int)k, 0.0f);
        eng_press(100 + (int)k, 0.0f);
        eng_press(200 + (int)k, 0.0f);
        ui.prs[k] = 0;
    }
    root_update();
    deep_update();
}

static void holds_changed(void)          /* DRONE or LOW HOLD switched: what no longer holds lets go */
{
    uint32_t keep = 0;
    if (holds(SIDE_L))
        keep |= side_mask(SIDE_L);
    if (holds(SIDE_R))
        keep |= side_mask(SIDE_R);
    latched_release(~keep & ~ui.keys);
    ui.latched &= keep;
    ui.latched |= ui.keys & ui.sounding & keep;   /* the keys down now stay */
    root_update();
    deep_update();
    ui.head_redraw = 1;
}

static void all_off(void)
{
    eng_all_off();
    ui.latched = 0;
    ui.sounding = 0;
    ui.deep = 0.0f;
}

/* ------------------------------------------------------------------ pages --- */
/* the knobs of SPLIT and PRESSURE: a value, its range, its text */
static const char *const PAGE_NAME[NPAGE] = {"MAIN", "SPLIT", "PRESSURE"};
static const char *const PK_NAME[2][4] = {{"SPLIT", "POINT", "SCALE", "FOLLOW"},
                                          {"PRESSURE", "AMOUNT", "TONE", "VIBRATO"}};

static int32_t pk_get(int pg, int k)
{
    if (pg == PG_SPLIT)
        return k == 0 ? set.split_oct : k == 1 ? set.split_key : k == 2 ? set.scale : set.follow;
    return k == 0 ? set.press : k == 1 ? set.pamt : k == 2 ? set.tone : set.vib;
}

static void pk_range(int pg, int k, int32_t *lo, int32_t *hi)
{
    static const int8_t LO[2][4] = {{0, 1, 0, 0}, {0, 0, -50, 0}};
    static const int8_t HI[2][4] = {{3, NKEYS - 1, NSCALE - 1, NFOLLOW - 1}, {NPRESS - 1, 100, 50, 100}};
    *lo = LO[pg - 1][k];
    *hi = HI[pg - 1][k];
}

static void pk_text(int pg, int k, char *t)
{
    int32_t v = pk_get(pg, k);
    t[0] = 0;
    if (pg == PG_SPLIT) {
        if (k == 0) {
            if (!v)
                text_cat(t, "OFF");
            else {
                signed_text(-v, t);
                text_cat(t, " OCT");
            }
        } else if (k == 1)
            note_text(53 + v, t);
        else if (k == 2)
            text_cat(t, SCALES[v].name);
        else
            text_cat(t, FOLLOW_NAME[v]);
    } else {
        if (k == 0)
            text_cat(t, PRESS_NAME[v]);
        else if (k == 2)
            signed_text(v, t);
        else {
            int_text(v, t);
            text_cat(t, "%");
        }
    }
}

static void pk_set(int pg, int k, int32_t v)
{
    char t[16];
    if (pg == PG_SPLIT) {
        if (k == 0) {
            if (set.split_oct && !v)     /* the split off: the low part's hold goes */
                ui.lowhold = 0;
            set.split_oct = (uint8_t)v;
        } else if (k == 1)
            set.split_key = (uint8_t)v;
        else if (k == 2)
            set.scale = (uint8_t)v;
        else
            set.follow = (uint8_t)v;
        if (k <= 1)
            holds_changed();
    } else {
        if (k == 0)
            set.press = (uint8_t)v;
        else if (k == 1)
            set.pamt = (uint8_t)v;
        else if (k == 2)
            set.tone = (int8_t)v;
        else
            set.vib = (uint8_t)v;
        sound();
    }
    dirty();
    pk_text(pg, k, t);
    app_say(PK_NAME[pg - 1][k], t);
}

static void page_knob(int k, int32_t e)
{
    int pg = ui.page;
    int32_t lo, hi, v = pk_get(pg, k), list = (pg == PG_SPLIT && k != 1) || (pg == PG_PRESS && k == 0);
    pk_range(pg, k, &lo, &hi);
    if (list) {                          /* a list: two detents a step */
        ui.enc_acc[k] += e;
        if (ui.enc_acc[k] > -2 && ui.enc_acc[k] < 2)
            return;
        e = ui.enc_acc[k] > 0 ? 1 : -1;
        ui.enc_acc[k] = 0;
    } else
        e = accel(EN_K1 + k, e, 2, 4);
    v = clamp(v + e, lo, hi);
    ui.knob_at[k] = plat_ms();
    if (v != pk_get(pg, k))
        pk_set(pg, k, v);
    ui.redraw = 1;
}

static void page_go(int pg)
{
    ui.page = (uint8_t)pg;
    ui.zone_until = 0;
    app_say(PAGE_NAME[pg], pg == PG_MAIN ? PR.name : "");
}

/* ------------------------------------------------------------------ knobs --- */
static void knob(int k, int32_t e)
{
    const macro_t *mc = &MACROS[k];
    int32_t p, v;
    int zb, za;
    if (ui.page != PG_MAIN) {
        page_knob(k, e);
        return;
    }
    p = clamp(knob_pos[k] + accel(EN_K1 + k, e, 3, 6), 0, pos_max(mc));
    v = val_of_pos(mc, p);
    zb = zone_at(mc, KM[k]);
    za = zone_at(mc, v);
    knob_pos[k] = p;
    ui.knob_at[k] = plat_ms();
    ui.redraw = 1;
    if (v == KM[k])
        return;
    KM[k] = (uint8_t)v;
    sound();
    dirty();
    if (za >= 0 && za != zb) {
        ui.zone_k = (int8_t)k;
        ui.zone_z = (int8_t)za;
        ui.zone_until = plat_ms() + 700u;
        if (ann_on())
            ann_say(mc->zone[za], 0, ANN_NOW);
    }
}

static void preset_load(int32_t d)
{
    set.preset = (uint8_t)(((int32_t)set.preset + d % NPRESETS + NPRESETS) % NPRESETS);
    knobs_from_settings();
    sound();
    dirty();
    ui.zone_until = 0;
    app_say("PRESET", PR.name);
}

static void octave(int d)
{
    int32_t o = clamp(set.octave + d, -2, 1);
    char t[8];
    if (o == set.octave)
        return;
    set.octave = (int8_t)o;
    dirty();
    signed_text(o, t);
    app_say("OCTAVE", t);
    ui.head_redraw = 1;
}

static void pressure(void)               /* the held keys' pressure, to their notes (as it changes) */
{
    uint32_t k;
    if (set.press == PRESS_OFF)
        return;
    for (k = 0; k < NKEYS; k++) {
        uint8_t pr = (ui.keys >> k & 1u) && (ui.sounding >> k & 1u) ? touch_pressure(k) : 0u;
        int d = (int)pr - (int)ui.prs[k];
        if (d >= 3 || d <= -3 || (pr == 0u && ui.prs[k])) {
            float f = (float)pr / 127.0f;
            ui.prs[k] = pr;
            eng_press((int)k, f);
            eng_press(100 + (int)k, f);
            eng_press(200 + (int)k, f);
        }
    }
}

static void input(void)
{
    uint32_t keys, btn = plat_buttons(), k, now = plat_ms(), down;
    int32_t e;
    touch_poll();
    keys = touch_keys();
    for (k = 0; k < NKEYS; k++)
        if (!(keys >> k & 1u) && (ui.keys >> k & 1u)) {
            ui.keys &= ~(1u << k);
            key_release(k);
        }
    down = ui.keys & keys;
    for (k = 0; k < NKEYS; k++) {        /* key 0 is F3 (MIDI 53) */
        uint32_t bit = 1u << k;
        if ((keys & bit) && !(ui.keys & bit)) {
            int v = touch_vel(k);
            if (v <= 0)
                v = 100;
            if (btn >> B_SEL & 1u) {     /* SEL held: the split here (this key the right's first) */
                if (k >= 1u) {
                    char t[8];
                    if (!set.split_oct)
                        set.split_oct = 1;
                    set.split_key = (uint8_t)k;
                    holds_changed();
                    dirty();
                    note_text(53 + (int32_t)k, t);
                    app_say("SPLIT", t);
                }
                ui.sel_used = 1;
            } else
                key_press(k, (uint8_t)v, down);
            down |= bit;
            if (ui.intro)
                ui.intro = 2;
        }
    }
    if (keys != ui.keys)
        ui.redraw = 1;
    ui.keys = keys;
    pressure();
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        if (ui.intro)
            ui.intro = 2;
        if (k == B_PLAY) {
            ui.drone = (uint8_t)!ui.drone;
            holds_changed();
            app_say("DRONE", ui.drone ? "ON" : "OFF");
        } else if (k == B_SEQ) {
            ui.lowhold = (uint8_t)!ui.lowhold;
            if (ui.lowhold && !set.split_oct) {   /* LOW HOLD needs the split: on, an octave down */
                set.split_oct = 1;
                dirty();
            }
            holds_changed();
            app_say("LOW HOLD", ui.lowhold ? "ON" : "OFF");
        } else if (k == B_ARP) {
            set.epic = (uint8_t)!set.epic;
            dirty();
            app_say("EPIC", set.epic ? "ON" : "OFF");
            ui.head_redraw = 1;
        } else if (k == B_OCTDN || k == B_OCTUP)
            octave(k == B_OCTUP ? 1 : -1);
        else if (k == B_REC)
            ui.rec_t = now;
        else if (k == B_HOME) {
            ui.home_t = now;
            if (ui.page != PG_MAIN)
                page_go(PG_MAIN);
        } else if (k == B_SEL) {
            ui.sel_t = now;
            ui.sel_used = 0;
        } else if (k == B_GLO)
            page_go(ui.page == PG_PRESS ? PG_MAIN : PG_PRESS);
    }
    if (ui.sel_t && !(btn >> B_SEL & 1u)) {   /* SEL let go: the SPLIT page (again: MAIN), unless it set the split */
        ui.sel_t = 0;
        if (!ui.sel_used)
            page_go(ui.page == PG_SPLIT ? PG_MAIN : PG_SPLIT);
    }
    if (ui.rec_t) {
        if (!(btn >> B_REC & 1u))
            ui.rec_t = 0;
        else if (now - ui.rec_t >= 1000u) {
            ui.rec_t = 0;
            all_off();
            app_say("ALL NOTES", "OFF");
        }
    }
    if (ui.home_t) {
        if (!(btn >> B_HOME & 1u))
            ui.home_t = 0;
        else if (now - ui.home_t >= 1000u) {
            ui.home_t = 0;
            for (k = 0; k < 4u; k++)
                KM[k] = PR.m[k];
            knobs_from_settings();
            sound();
            dirty();
            app_say(PR.name, "RESET");
        }
    }
    for (k = 0; k < 4u; k++)
        if ((e = plat_enc(EN_K1 + (int)k)) != 0) {
            knob((int)k, e);
            if (ui.intro)
                ui.intro = 2;
        }
    if ((e = plat_enc(EN_SELECT)) != 0) {
        char t[8];
        set.tone = (int8_t)clamp((int32_t)set.tone + accel(EN_SELECT, e, 2, 4), -50, 50);
        sound();
        dirty();
        signed_text(set.tone, t);
        app_say("TONE", t);
    }
    if ((e = plat_enc(EN_ALGO)) != 0) {
        char t[8];
        set.vib = (uint8_t)clamp((int32_t)set.vib + accel(EN_ALGO, e, 2, 4), 0, 100);
        sound();
        dirty();
        int_text(set.vib, t);
        text_cat(t, "%");
        app_say("VIBRATO", t);
    }
    if ((e = plat_enc(EN_PRESET)) != 0)
        preset_load(e > 0 ? 1 : -1);
    if (btn != ui.btn)
        ui.redraw = 1;
    ui.btn = btn;
}

static void lights(void)
{
    uint32_t btn = 0, dimb;
    if (ui.drone)
        btn |= LED_PLAY_GREEN;
    if (ui.lowhold)
        btn |= 1u << B_SEQ;
    if (set.epic)
        btn |= 1u << B_ARP;
    if (ui.page == PG_SPLIT)
        btn |= 1u << B_SEL;
    if (ui.page == PG_PRESS)
        btn |= 1u << B_GLO;
    if (set.octave < 0)
        btn |= 1u << B_OCTDN;
    if (set.octave > 0)
        btn |= 1u << B_OCTUP;
    dimb = (1u << B_PLAY) | (1u << B_ARP) | (1u << B_SEQ) | (1u << B_SEL) | (1u << B_GLO) | (1u << B_OCTDN) |
           (1u << B_OCTUP) | (1u << B_REC) | (1u << B_HOME);
    plat_lights(btn | (ui.btn & ~(1u << B_PLAY)), ui.sounding | ui.keys, dimb, side_mask(SIDE_L), 1);
}

void app_input_only(void) { input(); }

/* ------------------------------------------------------------------ drawing --- */
static void ctext(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}

static uint16_t mix565(uint16_t a, uint16_t b, uint32_t t)   /* a .. b, t 0..256 */
{
    int32_t ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int32_t br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31, tt = (int32_t)(t > 256u ? 256u : t);
    return (uint16_t)((((ar + ((br - ar) * tt >> 8)) & 31) << 11) | (((ag + ((bg - ag) * tt >> 8)) & 63) << 5) |
                      ((ab + ((bb - ab) * tt >> 8)) & 31));
}

/* rain, in Japanese (a neon sign): 13 x 10 */
static const char *const KANJI_AME[10] = {
    "#############", "      #      ", "#############", "#     #     #", "#  #  #  #  #",
    "#   # #   # #", "#     #     #", "#  #  #  #  #", "#   # #   # #", "#     #   ###",
};
static void draw_kanji(int32_t x, int32_t y, int32_t sc, uint16_t c)
{
    int32_t r, i;
    for (r = 0; r < 10; r++)
        for (i = 0; i < 13; i++)
            if (KANJI_AME[r][i] == '#')
                cv_rect(x + i * sc, y + r * sc, sc, sc, c);
}

static void neon_text(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, int lit)
{
    int32_t x = cx - text_w(f, s) / 2;
    if (!lit) {
        cv_text(x, y, f, s, RGB(30, 40, 60));
        return;
    }
    cv_text(x + 2, y + 2, f, s, RGB(120, 20, 90));
    cv_text(x - 1, y, f, s, RGB(20, 90, 120));
    cv_text(x, y, f, s, C_CYAN);
}

#include "city.inc"                      /* the film: the scenes, the rain, the cuts */

static void draw_head(void)
{
    char t[8];
    int lit = !(film.flicker && (film.frame & 4u));
    int32_t x = 6;
    cv_begin(240, 24, C_BLACK);
    cv_text(x + 1, 5, &FONT_B, "OFF", RGB(120, 20, 90));
    cv_text(x, 4, &FONT_B, "OFF", C_CYAN);
    x += text_w(&FONT_B, "OFF");
    cv_text(x + 1, 5, &FONT_B, "WORLD", lit ? RGB(120, 20, 90) : C_BLACK);
    cv_text(x, 4, &FONT_B, "WORLD", lit ? C_CYAN : RGB(30, 50, 70));
    cv_text(100, 7, &FONT_XS, "DRONE", ui.drone ? C_AMBER : RGB(60, 50, 50));
    cv_text(136, 7, &FONT_XS, "LOW", ui.lowhold ? C_AMBER : RGB(60, 50, 50));
    cv_text(160, 7, &FONT_XS, "EPIC", set.epic ? C_MAG : RGB(60, 40, 55));
    t[0] = 'O';
    t[1] = 'C';
    t[2] = 'T';
    signed_text(set.octave, t + 3);
    cv_text(236 - text_w(&FONT_XS, t), 7, &FONT_XS, t, RGB(150, 150, 170));
    cv_rect(0, 23, 240, 1, RGB(60, 20, 60));
    cv_blit(0, 0);
}

static void draw_knob(int k, int32_t x, int32_t y, uint32_t now)
{
    const macro_t *mc = &MACROS[k];
    int32_t v = KM[k], w = 54, z = zone_at(mc, v), zn = zone_near(mc, v), fill = (v * (w - 2)) / 127;
    uint32_t i;
    int hot = ui.knob_at[k] && now - ui.knob_at[k] < 600u;
    char t[8];
    cv_text(x, y, &FONT_XS, mc->name, hot ? C_WHITE : RGB(140, 140, 160));
    int_text(v, t);
    cv_text(x + w - text_w(&FONT_XS, t), y, &FONT_XS, t, hot ? C_CYAN : C_AMBER);
    cv_rect(x, y + 14, w, 8, RGB(50, 30, 60));
    cv_rect(x + 1, y + 15, w - 2, 6, C_BLACK);
    cv_rect(x + 1, y + 15, fill, 6, z >= 0 ? C_CYAN : C_MAG);
    for (i = 0; i < mc->nzones; i++) {
        int32_t tx = x + 1 + (mc->centre[i] * (w - 3)) / 127;
        cv_rect(tx, y + 11, 1, 3, (int)i == z ? C_CYAN : RGB(110, 90, 120));
    }
    ctext(x + w / 2, y + 26, &FONT_XS, mc->zone[z >= 0 ? z : zn], z >= 0 ? C_WHITE : RGB(90, 80, 100));
}

static void draw_pknob(int k, int32_t x, int32_t y, uint32_t now)   /* a page's knob: its name, its value */
{
    int32_t lo, hi, v = pk_get(ui.page, k), w = 54;
    int hot = ui.knob_at[k] && now - ui.knob_at[k] < 600u;
    char t[16];
    pk_range(ui.page, k, &lo, &hi);
    cv_text(x, y, &FONT_XS, PK_NAME[ui.page - 1][k], hot ? C_WHITE : RGB(140, 140, 160));
    cv_rect(x, y + 14, w, 8, RGB(50, 30, 60));
    cv_rect(x + 1, y + 15, w - 2, 6, C_BLACK);
    cv_rect(x + 1, y + 15, (v - lo) * (w - 2) / (hi - lo > 0 ? hi - lo : 1), 6, hot ? C_CYAN : C_MAG);
    pk_text(ui.page, k, t);
    ctext(x + w / 2, y + 26, &FONT_XS, t, hot ? C_CYAN : C_AMBER);
}

static void draw_panel(void)
{
    uint32_t now = plat_ms(), i;
    char t[40], n[8];
    cv_begin(240, 96, C_BLACK);
    cv_rect(0, 0, 240, 1, RGB(60, 20, 60));
    if (now < ui.msg_until) {
        int32_t x = cv_text(6, 5, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 8, 5, &FONT_B, ui.msg[1], C_CYAN);
    } else if (ui.page != PG_MAIN) {
        cv_text(6, 5, &FONT_B, "//", C_MAG);
        cv_text(28, 5, &FONT_B, PAGE_NAME[ui.page], C_AMBER);
    } else {
        t[0] = '>';
        t[1] = ' ';
        t[2] = (char)('1' + set.preset);
        t[3] = ' ';
        t[4] = 0;
        cv_text(6, 5, &FONT_B, t, C_MAG);
        cv_text(6 + text_w(&FONT_B, t), 5, &FONT_B, PR.name, C_AMBER);
    }
    if (ui.page == PG_MAIN && now < ui.zone_until && ui.zone_k >= 0) {
        const macro_t *mc = &MACROS[ui.zone_k];
        ctext(120, 28, &FONT_XS, mc->name, RGB(140, 140, 160));
        ctext(120, 42, &FONT_M, mc->zone[ui.zone_z], C_CYAN);
        cv_rect(60, 70, 120, 1, C_MAG);
    } else {
        for (i = 0; i < 4u; i++) {
            if (ui.page == PG_MAIN)
                draw_knob((int)i, 4 + (int32_t)i * 59, 28, now);
            else
                draw_pknob((int)i, 4 + (int32_t)i * 59, 28, now);
        }
    }
    t[0] = 0;
    if (split_on() && set.follow != FOLLOW_OFF) {   /* the melody's key */
        text_cat(t, NOTE_NAME[ui.root]);
        text_cat(t, " ");
        text_cat(t, SCALES[set.scale].name);
        text_cat(t, "  ");
    } else {
        text_cat(t, "VOICES ");
        int_text(eng_voices(), n);
        text_cat(t, n);
        text_cat(t, "/8  ");
    }
    text_cat(t, "CPU ");
    int_text((int32_t)plat_cpu_pct(), n);
    text_cat(t, n);
    text_cat(t, "%");
    cv_text(6, 80, &FONT_XS, t, RGB(40, 140, 150));
    if (set.press != PRESS_OFF) {        /* the pressure: the hardest-pressed key */
        uint32_t k, pmax = 0;
        for (k = 0; k < NKEYS; k++)
            if (ui.prs[k] > pmax)
                pmax = ui.prs[k];
        cv_rect(200, 82, 34, 6, RGB(50, 30, 60));
        cv_rect(201, 83, (int32_t)(pmax * 32u / 127u), 4, C_MAG);
    }
    cv_blit(0, 144);
}

static void draw_intro(uint32_t ms)
{
    int lit;
    cv_begin(240, 144, C_BLACK);
    lit = ms > 1300u || ((ms / 70u) * 2654435761u >> 29) > 4u;
    if (ms > 500u)
        draw_kanji(101, 4, 3, ms > 900u ? C_MAG : RGB(80, 20, 60));
    neon_text(120, 50, &APP_BIG_FONT, "OFFWORLD", ms > 400u && lit);
    if (ms > 1600u)
        ctext(120, 92, &FONT_XS, "by swtdr34mz", RGB(150, 150, 170));
    if (ms > 300u) {
        uint32_t i, w = ms > 1800u ? 240u : (ms - 300u) * 240u / 1500u;
        for (i = 0; i < 6u; i++)
            cv_rect(120 - (int32_t)w / 2, 120 + (int32_t)i, (int32_t)w, 1, mix565(RGB(150, 64, 36), C_BLACK, i * 40u));
    }
    cv_blit(0, 48);
}

/* ------------------------------------------------------------- the contract --- */
void app_init(void)
{
    eng_init();
    plat_cpu2_set(1);                    /* half the voices on the second core (one core still works) */
    settings_load();
    knobs_from_settings();
    sound();
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    eng_render(lr, n, (float)master_q12 * (1.0f / 4096.0f) * 8300000.0f);
}

void app_panic(void)
{
    eng_panic();
    ui.latched = ui.sounding = 0;
    ui.deep = 0.0f;
}

void app_overload(void) { eng_shed(); }

void app_splash(void)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_intro(2000u);
}

void app_ui_init(void)
{
    touch_on(1);
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.rnd = 0x51ED270Bu;
    film_init();
    ui.zone_k = -1;
    ui.redraw = ui.head_redraw = 1;
    ui.intro = 1;
    ui.intro_t0 = plat_ms();
}

void app_frame(void)
{
    uint32_t now = plat_ms(), pkt, k;
    int port;
    input();
    lights();
    while (plat_midi_in(&pkt, &port)) {  /* MIDI in plays the brass (any channel) */
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
        int id = MIDI_ID + (int)d1;
        (void)port;
        if ((st & 0xF0u) == 0x90u && d2) {
            eng_note_on(id, (float)d1, (float)d2 / 127.0f);
            if (set.epic) {
                eng_note_on(id + 100, (float)d1 + 7.0f, (float)d2 / 127.0f * 0.85f);
                eng_note_on(id + 200, (float)d1 + 12.0f, (float)d2 / 127.0f * 0.8f);
            }
        } else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u) {
            eng_note_off(id);
            eng_note_off(id + 100);
            eng_note_off(id + 200);
        } else if ((st & 0xF0u) == 0xA0u) {   /* polyphonic pressure: as the keys' */
            eng_press(id, (float)d2 / 127.0f);
            eng_press(id + 100, (float)d2 / 127.0f);
            eng_press(id + 200, (float)d2 / 127.0f);
        } else if ((st & 0xF0u) == 0xB0u && (d1 == 123u || d1 == 120u))
            eng_all_off();
    }
    if (ann_on() && ann_where()) {
        char t[64] = "OFFWORLD. ";
        text_cat(t, PR.name);
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(now);
    if (set_dirty && now - dirty_at > 4000u)
        settings_save();
    if (ui.intro) {
        uint32_t ms = now - ui.intro_t0;
        if (ui.intro == 1 && ms < 2800u) {
            if (now - ui.scene_at >= 33u) {
                ui.scene_at = now;
                draw_intro(ms);
            }
            return;
        }
        ui.intro = 0;
        lcd_fill(0, 0, 240, 240, C_BLACK);
        ui.redraw = ui.head_redraw = 1;
        film_cut(now);
    }
    if ((ui.msg_until && now >= ui.msg_until) || (ui.zone_until && now >= ui.zone_until)) {
        if (now >= ui.msg_until)
            ui.msg_until = 0;
        if (now >= ui.zone_until)
            ui.zone_until = 0;
        ui.redraw = 1;
    }
    if (now - ui.scene_at >= 40u) {      /* the film at 25 frames a second */
        uint8_t fl = film.flicker;
        ui.scene_at = now;
        for (k = 0; k < NKEYS; k++) {    /* the lights follow the notes, and fade */
            if (ui.sounding >> k & 1u)
                ui.win[k] = 255;
            else if (ui.win[k])
                ui.win[k] = (uint8_t)(ui.win[k] > 6u ? ui.win[k] - 6u : 0u);
        }
        film_frame(now);
        if (fl || film.flicker)
            ui.head_redraw = 1;
        if ((film.frame & 7u) == 0u)
            ui.redraw = 1;               /* the voices, the CPU */
    }
    if (ui.head_redraw) {
        ui.head_redraw = 0;
        draw_head();
    }
    if (ui.redraw) {
        ui.redraw = 0;
        draw_panel();
    }
}

/* ------------------------------------------------------------- backup ------ */
int app_backup_playing(void) { return ui.sounding != 0; }
void app_backup_flush(void)
{
    if (set_dirty)
        settings_save();
}
void app_backup_hold(int on) { (void)on; }
void app_backup_restored(void)
{
    settings_load();
    knobs_from_settings();
    sound();
    ui.redraw = ui.head_redraw = 1;
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "preset"))
        *got = set.preset;
    else if (!strcmp(what, "drone"))
        *got = ui.drone;
    else if (!strcmp(what, "lowhold"))
        *got = ui.lowhold;
    else if (!strcmp(what, "epic"))
        *got = set.epic;
    else if (!strcmp(what, "octave"))
        *got = set.octave;
    else if (!strcmp(what, "tone"))
        *got = set.tone;
    else if (!strcmp(what, "vib"))
        *got = set.vib;
    else if (!strcmp(what, "page"))
        *got = ui.page;
    else if (!strcmp(what, "split"))
        *got = set.split_oct ? set.split_key : 0;
    else if (!strcmp(what, "split_oct"))
        *got = set.split_oct;
    else if (!strcmp(what, "scale"))
        *got = set.scale;
    else if (!strcmp(what, "follow"))
        *got = set.follow;
    else if (!strcmp(what, "root"))
        *got = ui.root;
    else if (!strcmp(what, "press"))
        *got = set.press;
    else if (!strcmp(what, "pamt"))
        *got = set.pamt;
    else if (!strncmp(what, "kp", 2) && what[2])   /* kp0..kp26: the pitch a key started at */
        *got = (int)ui.kp[atoi(what + 2) % NKEYS];
    else if (!strncmp(what, "prs", 3) && what[3])
        *got = ui.prs[atoi(what + 3) % NKEYS];
    else if (!strcmp(what, "voices"))
        *got = eng_voices();
    else if (!strcmp(what, "notes"))
        *got = (int)eng_notes();
    else if (!strcmp(what, "thunder"))
        *got = (int)eng_thunder();
    else if (!strcmp(what, "latched"))
        *got = (int)ui.latched;
    else if (!strcmp(what, "sounding"))
        *got = (int)ui.sounding;
    else if (!strcmp(what, "deep"))
        *got = (int)ui.deep;
    else if (!strcmp(what, "intro"))
        *got = ui.intro;
    else if (!strcmp(what, "scene"))
        *got = film.scene;
    else if (!strcmp(what, "dirty"))
        *got = set_dirty;
    else if (what[0] == 'm' && what[1] >= '1' && what[1] <= '4' && !what[2])
        *got = KM[what[1] - '1'];
    else if (!strncmp(what, "zone", 4) && what[4] >= '1' && what[4] <= '4')
        *got = zone_at(&MACROS[what[4] - '1'], KM[what[4] - '1']);
    else
        return 0;
    return 1;
}
#endif
