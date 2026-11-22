/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (OFFWORLD) */
/* OFFWORLD: input, the knobs and their sweetspots, DRONE and EPIC, the city on the screen, settings. Part of the
 * unity build (device/unity.c, host/plat_host.c), after the platform: gfx.c and announce.c are in scope. The sound is
 * dsp/engine.c.
 *
 * Controls (v0.1)
 *   the keys        the brass, with velocity (harder = brighter); OCT- / OCT+ the octave
 *   K1..K4          SWELL, DRIFT, SPACE, RAIN: four macros. Each moves several parameters at once; turned slowly it
 *                   rests a few detents on each sweetspot (its name lights up); turned fast it runs past them
 *   PRESET          the preset (each keeps its own knobs); HOME held 1 s: the preset's knobs back as they came
 *   SELECT          TONE (darker / brighter)      ALGORITHM  VIBRATO (comes in slowly, as a player's would)
 *   PLAY            DRONE on / off: the notes stay; a key pressed with no key down starts a new chord; a deep root
 *                   an octave below the lowest note sounds under it
 *   ARP             EPIC on / off: one finger plays root, fifth and octave
 *   REC held 1 s    every note let go
 *   Settings save themselves a few seconds after the last change (DRONE starts off). */
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
    uint8_t rsv[7];
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

/* --------------------------------------------------------------- settings --- */
static void settings_defaults(void)
{
    uint32_t i, k;
    memset(&set, 0, sizeof set);
    set.magic = SET_MAGIC;
    for (i = 0; i < NPRESETS; i++)
        for (k = 0; k < 4u; k++)
            set.m[i][k] = PRESETS[i].m[k];
    set.vib = 30;
}

static int settings_ok(void)
{
    uint32_t i, k;
    for (i = 0; i < NPRESETS; i++)
        for (k = 0; k < 4u; k++)
            if (set.m[i][k] > 127u)
                return 0;
    return set.magic == SET_MAGIC && set.preset < NPRESETS && set.tone >= -50 && set.tone <= 50 && set.vib <= 100u &&
           set.octave >= -2 && set.octave <= 1 && set.epic <= 1u;
}

static void settings_load(void)
{
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || !settings_ok())
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

static void sound(void) { eng_sound(set.preset, KM, (float)set.tone / 50.0f, (float)set.vib / 100.0f); }

/* ------------------------------------------------------- knobs: sweetspots --- */
/* A knob's position runs past each sweetspot zone REST detents more than its value: turned slowly, it rests on the
 * zone for those detents (a soft notch); turned fast, it runs past (the acceleration takes several at once). */
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
#define NDROPS 90
#define NFAR 60
static struct {
    char msg[2][24];
    uint32_t msg_until;
    int8_t zone_k, zone_z;
    uint32_t zone_until;
    uint32_t knob_at[4];
    uint32_t keys, btn;                  /* held last frame */
    uint32_t latched;                    /* DRONE: the keys whose notes stay */
    uint32_t sounding;                   /* keys whose notes sound now (held or latched) */
    float kp[NKEYS];                     /* the pitch each key started at */
    uint8_t kv[NKEYS];
    uint8_t drone;
    float deep;                          /* the deep root's pitch (0: none) */
    uint32_t rec_t, home_t;
    uint32_t enc_at[NE];
    uint8_t redraw, head_redraw;
    /* the intro */
    uint8_t intro;
    uint32_t intro_t0;
    /* the city */
    uint32_t scene_at, frame;
    uint8_t win[NKEYS];                  /* each window's light 0..255 */
    float drop_x[NDROPS], drop_y[NDROPS], drop_v[NDROPS];
    uint8_t far_h[NFAR], far_win[NFAR];
    uint16_t sky[88];
    float blimp_x;
    float car_x, car_y, car_v;
    uint8_t car_on;
    uint32_t notes_seen, thunder_seen;
    uint8_t flash;
    int16_t bolt[8];
    uint8_t flicker;
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
static void key_start(uint32_t k, uint8_t vel)
{
    float p = 53.0f + (float)k + 12.0f * (float)set.octave, v = (float)vel / 127.0f;
    ui.kp[k] = p;
    ui.kv[k] = vel;
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
}

/* the deep root under a drone: an octave below the lowest note that stays */
static void deep_update(void)
{
    float want = 0.0f;
    uint32_t k;
    if (ui.drone)
        for (k = 0; k < NKEYS; k++)
            if (ui.latched >> k & 1u) {
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

static void latched_release(uint32_t keep)   /* the latched notes let go, but those in keep */
{
    uint32_t k;
    for (k = 0; k < NKEYS; k++)
        if ((ui.latched >> k & 1u) && !(keep >> k & 1u))
            key_stop(k);
    ui.latched &= keep;
}

static void key_press(uint32_t k, uint8_t vel, uint32_t down_before)
{
    if (ui.drone) {
        if (!down_before)                /* a key with no key down: a new chord */
            latched_release(0);
        ui.latched |= 1u << k;
    }
    key_start(k, vel);
    if (ui.drone)
        deep_update();
}

static void key_release(uint32_t k)
{
    if (!(ui.drone && (ui.latched >> k & 1u)))
        key_stop(k);
}

static void drone_toggle(void)
{
    ui.drone = (uint8_t)!ui.drone;
    if (ui.drone)
        ui.latched = ui.keys & ui.sounding;   /* the keys down now stay */
    else
        latched_release(ui.keys);
    if (!ui.drone)
        ui.latched = 0;
    deep_update();
    app_say("DRONE", ui.drone ? "ON" : "OFF");
    ui.head_redraw = 1;
}

static void all_off(void)
{
    eng_all_off();
    ui.latched = 0;
    ui.sounding = 0;
    ui.deep = 0.0f;
}

/* ------------------------------------------------------------------ knobs --- */
static void knob(int k, int32_t e)
{
    const macro_t *mc = &MACROS[k];
    int32_t p = clamp(knob_pos[k] + accel(EN_K1 + k, e, 3, 6), 0, pos_max(mc)), v = val_of_pos(mc, p);
    int zb = zone_at(mc, KM[k]), za = zone_at(mc, v);
    knob_pos[k] = p;
    ui.knob_at[k] = plat_ms();
    ui.redraw = 1;
    if (v == KM[k])
        return;
    KM[k] = (uint8_t)v;
    sound();
    dirty();
    if (za >= 0 && za != zb) {           /* a sweetspot reached: its name, big */
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
    ui.head_redraw = 1;
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

static void input(void)
{
    uint32_t keys, btn = plat_buttons(), k, now = plat_ms(), down;
    int32_t e;
    touch_poll();
    keys = touch_keys();
    for (k = 0; k < NKEYS; k++)          /* the releases first, then the presses, each knowing the keys down before */
        if (!(keys >> k & 1u) && (ui.keys >> k & 1u))
            key_release(k);
    down = ui.keys & keys;
    for (k = 0; k < NKEYS; k++) {        /* key 0 is F3 (MIDI 53) */
        uint32_t bit = 1u << k;
        if ((keys & bit) && !(ui.keys & bit)) {
            int v = touch_vel(k);
            if (v <= 0)
                v = 100;
            key_press(k, (uint8_t)v, down);
            down |= bit;
            if (ui.intro)
                ui.intro = 2;            /* a key ends the intro (and plays) */
        }
    }
    if (keys != ui.keys)
        ui.redraw = 1;
    ui.keys = keys;
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        if (ui.intro)
            ui.intro = 2;
        if (k == B_PLAY)
            drone_toggle();
        else if (k == B_ARP) {
            set.epic = (uint8_t)!set.epic;
            dirty();
            app_say("EPIC", set.epic ? "ON" : "OFF");
            ui.head_redraw = 1;
        } else if (k == B_OCTDN || k == B_OCTUP)
            octave(k == B_OCTUP ? 1 : -1);
        else if (k == B_REC)
            ui.rec_t = now;
        else if (k == B_HOME)
            ui.home_t = now;
    }
    if (ui.rec_t) {                      /* REC held a second: every note let go */
        if (!(btn >> B_REC & 1u))
            ui.rec_t = 0;
        else if (now - ui.rec_t >= 1000u) {
            ui.rec_t = 0;
            all_off();
            app_say("ALL NOTES", "OFF");
        }
    }
    if (ui.home_t) {                     /* HOME held a second: the preset's knobs as they came */
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
    if (set.epic)
        btn |= 1u << B_ARP;
    if (set.octave < 0)
        btn |= 1u << B_OCTDN;
    if (set.octave > 0)
        btn |= 1u << B_OCTUP;
    dimb = (1u << B_PLAY) | (1u << B_ARP) | (1u << B_OCTDN) | (1u << B_OCTUP) | (1u << B_REC) | (1u << B_HOME);
    plat_lights(btn | (ui.btn & ~(1u << B_PLAY)), ui.sounding | ui.keys, dimb, 0, 1);
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
    int32_t br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31, tt = (int32_t)t;
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

/* the title in neon: a magenta glow, the cyan tube over it */
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

/* the towers: x, width, height above the ground (each has 9 of the keys' windows: 3 x 3) */
#define GROUND 108
static const int16_t TW[3][3] = {{20, 38, 90}, {94, 48, 104}, {174, 36, 84}};

static void window_xy(uint32_t k, int32_t *x, int32_t *y)
{
    uint32_t t = k / 9u, i = k % 9u, col = i % 3u, row = i / 3u;
    int32_t tx = TW[t][0], tw = TW[t][1];
    *x = tx + 5 + (int32_t)col * ((tw - 10) / 3) + ((tw - 10) / 3 - 6) / 2;
    *y = GROUND - 18 - (int32_t)row * 16;
}

static void scene_init(void)
{
    uint32_t i;
    ui.rnd = 0x1234567u;
    for (i = 0; i < NFAR; i++) {
        ui.far_h[i] = (uint8_t)(16u + urand() % 42u);
        ui.far_win[i] = (uint8_t)urand();
    }
    for (i = 0; i < 88u; i++) {          /* the sky: night above, the smog's amber glow at the horizon */
        uint32_t t = i * i * 256u / (87u * 87u);
        ui.sky[i] = mix565(RGB(6, 4, 22), RGB(150, 64, 36), t);
    }
    for (i = 0; i < NDROPS; i++) {
        ui.drop_x[i] = (float)(urand() % 250u);
        ui.drop_y[i] = (float)(urand() % 120u);
        ui.drop_v[i] = 4.0f + (float)(urand() % 40u) * 0.1f;
    }
    ui.blimp_x = 150.0f;
}

/* the city at night, 240 x 120: sky, a blimp with the preset's name, far towers, three near ones with the keys'
 * windows, flames, a flying car, the wet street with the lights in it, the rain, lightning */
static void draw_scene(void)
{
    uint32_t i, k, now = plat_ms();
    int32_t y, x;
    float lvl = eng_level();
    uint32_t rain_n = (uint32_t)KM[3] * NDROPS / 127u;
    uint32_t flash = ui.flash;
    cv_begin(240, 120, C_BLACK);
    for (y = 0; y < 88; y++) {
        uint16_t c = ui.sky[y];
        if (flash >= 5u || flash == 3u)
            c = mix565(c, RGB(190, 180, 230), flash >= 5u ? 170u : 90u);
        cv_rect(0, y, 240, 1, c);
    }
    /* the far towers */
    for (i = 0; i < NFAR; i++) {
        int32_t h = ui.far_h[i];
        x = (int32_t)i * 4;
        cv_rect(x, GROUND - h, 4, h, RGB(22, 16, 42));
        if (ui.far_win[i] & 1u)
            cv_rect(x + 1, GROUND - h + 4 + (ui.far_win[i] >> 4), 1, 1, RGB(120, 80, 50));
        if ((ui.far_win[i] & 6u) == 6u)
            cv_rect(x + 2, GROUND - h + 10 + (ui.far_win[i] >> 5), 1, 1, RGB(90, 70, 90));
    }
    /* the near towers, their neon, their windows */
    for (i = 0; i < 3u; i++) {
        int32_t tx = TW[i][0], tw = TW[i][1], th = TW[i][2], ty = GROUND - th;
        cv_rect(tx, ty, tw, th, RGB(10, 10, 24));
        if (i == 1u) {                   /* the middle one steps in at the top */
            cv_rect(tx + 8, ty - 8, tw - 16, 8, RGB(10, 10, 24));
            cv_rect(tx + 18, ty - 18, tw - 36, 10, RGB(10, 10, 24));
            cv_rect(tx + 23, ty - 30, 2, 12, RGB(40, 40, 60));
            if ((ui.frame & 31u) < 10u)
                cv_rect(tx + 22, ty - 32, 4, 2, RGB(255, 40, 40));
            cv_rect(tx, ty, 1, th, C_MAG);
            cv_rect(tx + tw - 1, ty, 1, th, RGB(120, 30, 90));
        } else if (i == 0u) {
            cv_rect(tx + tw - 2, ty, 2, th, C_CYAN);
            cv_rect(tx + 4, ty - 10, 2, 10, RGB(40, 40, 60));
            if ((ui.frame & 63u) < 20u)
                cv_rect(tx + 3, ty - 12, 4, 2, RGB(255, 40, 40));
        } else {                         /* the sign on the right one: rain */
            int lit = !(ui.flicker && (ui.frame & 2u));
            cv_rect(tx - 15, ty + 6, 16, 33, RGB(30, 6, 26));
            draw_kanji(tx - 14, ty + 8, 1, lit ? C_MAG : RGB(70, 20, 60));
            draw_kanji(tx - 14, ty + 22, 1, lit ? C_CYAN : RGB(20, 60, 80));
        }
        for (y = ty + 6; y < GROUND - 4; y += 5)   /* the offices: small dim windows */
            for (x = tx + 3; x < tx + tw - 3; x += 4)
                if (((uint32_t)(x * 7 + y * 13) % 11u) < 2u)
                    cv_rect(x, y, 2, 2, RGB(50, 44, 40));
    }
    for (k = 0; k < NKEYS; k++) {        /* the keys' windows: lit while the note sounds, fading after */
        int32_t wx, wy;
        uint32_t b = ui.win[k];
        window_xy(k, &wx, &wy);
        cv_rect(wx, wy, 6, 5, b ? mix565(RGB(40, 30, 30), RGB(255, 200, 100), b) : RGB(28, 24, 34));
        if (b > 200u)
            cv_rect(wx - 1, wy - 1, 8, 1, RGB(255, 230, 160));
    }
    /* the blimp: a dark hull, its screen with the preset's name, two searchlights */
    {
        const char *nm = PR.name;
        int32_t w = text_w(&FONT_XS, nm) + 16, bx = (int32_t)ui.blimp_x, by = 10;
        cv_line(bx + w / 2, by + 16, bx + w / 2 - 30, 88, RGB(70, 52, 60));
        cv_line(bx + w / 2 + 6, by + 16, bx + w / 2 + 40, 88, RGB(70, 52, 60));
        cv_rect(bx + 4, by, w - 8, 18, RGB(28, 26, 40));
        cv_rect(bx, by + 3, w, 12, RGB(28, 26, 40));
        cv_rect(bx + 6, by + 3, w - 12, 12, RGB(40, 8, 40));
        cv_text(bx + 8, by + 3, &FONT_XS, nm, (ui.frame & 31u) < 29u ? C_MAG : RGB(140, 40, 110));
        cv_rect(bx + w / 2 - 4, by + 18, 8, 3, RGB(28, 26, 40));
        if ((ui.frame & 15u) < 8u)
            cv_rect(bx + w - 3, by + 7, 2, 2, RGB(255, 40, 40));
    }
    /* the flames on the stacks, higher when it sounds */
    for (i = 0; i < 2u; i++) {
        int32_t cx = i ? 229 : 6, fh = 3 + (int32_t)(urand() % 5u) + (int32_t)(fm_minf(lvl, 1.0f) * 22.0f);
        cv_rect(cx, GROUND - 26, 6, 26, RGB(30, 24, 36));
        cv_rect(cx + 1, GROUND - 26 - fh, 4, fh, C_FIRE);
        cv_rect(cx + 2, GROUND - 26 - fh * 2 / 3, 2, fh * 2 / 3, RGB(255, 220, 90));
        cv_rect(cx + (int32_t)(urand() % 4u), GROUND - 28 - fh - (int32_t)(urand() % 4u), 1, 2, RGB(255, 80, 20));
    }
    /* the flying car */
    if (ui.car_on) {
        int32_t cx = (int32_t)ui.car_x, cy = (int32_t)ui.car_y, d = ui.car_v > 0.0f ? 1 : -1;
        for (i = 1; i < 6u; i++)
            cv_rect(cx - d * (int32_t)i * 4, cy + 1, 3, 1, mix565(RGB(10, 10, 24), C_CYAN, 200u - i * 35u));
        cv_rect(cx - 3, cy, 7, 3, RGB(60, 60, 80));
        cv_rect(d > 0 ? cx + 4 : cx - 5, cy + 1, 2, 1, RGB(255, 255, 230));
        cv_rect(d > 0 ? cx - 4 : cx + 4, cy + 1, 1, 1, RGB(255, 40, 40));
    }
    /* the street, wet: the windows and the neon in it, rippling */
    cv_rect(0, GROUND, 240, 120 - GROUND, RGB(8, 6, 18));
    for (k = 0; k < NKEYS; k++)
        if (ui.win[k] > 30u) {
            int32_t wx, wy;
            window_xy(k, &wx, &wy);
            for (y = GROUND + 1; y < 120; y += 2)
                cv_rect(wx + (int32_t)((now / 90u + (uint32_t)y) % 3u) - 1, y, 5, 1,
                        mix565(RGB(8, 6, 18), RGB(255, 190, 90), ui.win[k] * (120u - (uint32_t)y) / 14u));
        }
    for (y = GROUND + 1; y < 120; y += 2) {
        cv_rect(TW[1][0] + (int32_t)((now / 120u + (uint32_t)y) % 2u), y, 2, 1, RGB(120, 30, 90));
        cv_rect(TW[0][0] + TW[0][1] - 2 + (int32_t)((now / 100u + (uint32_t)y) % 2u), y, 2, 1, RGB(30, 110, 130));
    }
    /* the rain */
    for (i = 0; i < rain_n; i++) {
        int32_t dx = (int32_t)ui.drop_x[i], dy = (int32_t)ui.drop_y[i], len = 4 + (int32_t)(ui.drop_v[i] * 0.6f);
        if (dy + len >= 119) {
            cv_rect(dx - 1, 118, 3, 1, RGB(90, 110, 140));   /* a splash */
            continue;
        }
        cv_line(dx, dy, dx - 1, dy + len, C_RAIN);
    }
    /* the lightning */
    if (flash >= 4u)
        for (i = 0; i + 1u < 8u; i++)
            cv_line(ui.bolt[i], (int32_t)i * 12, ui.bolt[i + 1], (int32_t)(i + 1u) * 12, RGB(240, 235, 255));
    cv_blit(0, 24);
}

/* the scene's movement, a frame */
static void scene_step(void)
{
    uint32_t i, k, rain_n = (uint32_t)KM[3] * NDROPS / 127u, th = eng_thunder(), nn = eng_notes();
    for (k = 0; k < NKEYS; k++) {
        if (ui.sounding >> k & 1u)
            ui.win[k] = 255;
        else if (ui.win[k])
            ui.win[k] = (uint8_t)(ui.win[k] > 6u ? ui.win[k] - 6u : 0u);
    }
    for (i = 0; i < rain_n; i++) {
        ui.drop_y[i] += ui.drop_v[i];
        ui.drop_x[i] -= ui.drop_v[i] * 0.15f;
        if (ui.drop_y[i] >= 120.0f) {
            ui.drop_y[i] = -(float)(urand() % 20u);
            ui.drop_x[i] = (float)(urand() % 250u);
        }
    }
    ui.blimp_x -= 0.35f;
    if (ui.blimp_x < -110.0f)
        ui.blimp_x = 245.0f;
    if (!ui.car_on && (nn != ui.notes_seen || (eng_level() > 0.05f && urand() % 600u == 0u))) {
        ui.car_on = 1;
        ui.car_v = (urand() & 1u) ? 2.6f : -2.6f;
        ui.car_x = ui.car_v > 0.0f ? -10.0f : 250.0f;
        ui.car_y = (float)(24u + urand() % 34u);
    }
    ui.notes_seen = nn;
    if (ui.car_on) {
        ui.car_x += ui.car_v;
        ui.car_y += 0.08f;
        if (ui.car_x < -20.0f || ui.car_x > 260.0f)
            ui.car_on = 0;
    }
    if (th != ui.thunder_seen) {         /* thunder: the sky lights up, a bolt */
        int32_t x = 30 + (int32_t)(urand() % 180u);
        ui.thunder_seen = th;
        ui.flash = 7;
        for (i = 0; i < 8u; i++) {
            ui.bolt[i] = (int16_t)x;
            x += (int32_t)(urand() % 15u) - 7;
        }
    } else if (ui.flash)
        ui.flash--;
    if (ui.flicker)
        ui.flicker--;
    else if (urand() % 240u == 0u)
        ui.flicker = (uint8_t)(6u + urand() % 10u);   /* the sign buzzes */
    ui.frame++;
}

static void draw_head(void)
{
    char t[8];
    cv_begin(240, 24, C_BLACK);
    {
        int lit = !(ui.flicker && (ui.frame & 4u));
        int32_t x = 6;
        cv_text(x + 1, 5, &FONT_B, "OFF", RGB(120, 20, 90));
        cv_text(x, 4, &FONT_B, "OFF", C_CYAN);
        x += text_w(&FONT_B, "OFF");
        cv_text(x + 1, 5, &FONT_B, "WORLD", lit ? RGB(120, 20, 90) : C_BLACK);
        cv_text(x, 4, &FONT_B, "WORLD", lit ? C_CYAN : RGB(30, 50, 70));
    }
    cv_text(110, 7, &FONT_XS, "DRONE", ui.drone ? C_AMBER : RGB(60, 50, 50));
    cv_text(150, 7, &FONT_XS, "EPIC", set.epic ? C_MAG : RGB(60, 40, 55));
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

static void draw_panel(void)
{
    uint32_t now = plat_ms(), i;
    char t[40];
    cv_begin(240, 96, C_BLACK);
    cv_rect(0, 0, 240, 1, RGB(60, 20, 60));
    if (now < ui.msg_until) {
        int32_t x = cv_text(6, 5, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 8, 5, &FONT_B, ui.msg[1], C_CYAN);
    } else {
        t[0] = '>';
        t[1] = ' ';
        t[2] = (char)('1' + set.preset);
        t[3] = ' ';
        t[4] = 0;
        cv_text(6, 5, &FONT_B, t, C_MAG);
        cv_text(6 + text_w(&FONT_B, t), 5, &FONT_B, PR.name, C_AMBER);
    }
    if (now < ui.zone_until && ui.zone_k >= 0) {
        const macro_t *mc = &MACROS[ui.zone_k];
        ctext(120, 28, &FONT_XS, mc->name, RGB(140, 140, 160));
        ctext(120, 42, &FONT_M, mc->zone[ui.zone_z], C_CYAN);
        cv_rect(60, 70, 120, 1, C_MAG);
    } else {
        for (i = 0; i < 4u; i++)
            draw_knob((int)i, 4 + (int32_t)i * 59, 28, now);
    }
    t[0] = 0;
    text_cat(t, "VOICES ");
    int_text(eng_voices(), t + 7);
    text_cat(t, "/8  TONE ");
    signed_text(set.tone, t + strlen(t));
    text_cat(t, "  VIB ");
    int_text(set.vib, t + strlen(t));
    cv_text(6, 80, &FONT_XS, t, RGB(40, 140, 150));
    cv_blit(0, 144);
}

/* the intro: the title buzzes on like a neon tube, rain in Japanese above it, the horizon glowing */
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
            cv_rect(120 - (int32_t)w / 2, 120 + (int32_t)i, (int32_t)w, 1,
                    mix565(RGB(150, 64, 36), C_BLACK, i * 40u));
    }
    cv_blit(0, 48);
}

/* ------------------------------------------------------------- the contract --- */
void app_init(void)
{
    eng_init();
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
    scene_init();
    ui.zone_k = -1;
    ui.redraw = ui.head_redraw = 1;
    ui.intro = 1;
    ui.intro_t0 = plat_ms();
}

void app_frame(void)
{
    uint32_t now = plat_ms(), pkt;
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
    }
    if ((ui.msg_until && now >= ui.msg_until) || (ui.zone_until && now >= ui.zone_until)) {
        if (now >= ui.msg_until)
            ui.msg_until = 0;
        if (now >= ui.zone_until)
            ui.zone_until = 0;
        ui.redraw = 1;
    }
    if (now - ui.scene_at >= 40u) {      /* the city at 25 frames a second */
        uint8_t fl = ui.flicker;
        ui.scene_at = now;
        scene_step();
        draw_scene();
        if (fl || ui.flicker)
            ui.head_redraw = 1;
        if ((ui.frame & 7u) == 0u)
            ui.redraw = 1;               /* the voices' count */
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
    else if (!strcmp(what, "epic"))
        *got = set.epic;
    else if (!strcmp(what, "octave"))
        *got = set.octave;
    else if (!strcmp(what, "tone"))
        *got = set.tone;
    else if (!strcmp(what, "vib"))
        *got = set.vib;
    else if (!strcmp(what, "voices"))
        *got = eng_voices();
    else if (!strcmp(what, "notes"))
        *got = (int)eng_notes();
    else if (!strcmp(what, "thunder"))
        *got = (int)eng_thunder();
    else if (!strcmp(what, "latched"))
        *got = (int)ui.latched;
    else if (!strcmp(what, "deep"))
        *got = (int)ui.deep;
    else if (!strcmp(what, "intro"))
        *got = ui.intro;
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
