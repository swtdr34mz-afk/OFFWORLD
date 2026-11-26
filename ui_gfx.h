/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (OFFWORLD; from BA-55) */
/* The drawing tools for the finer look: anti-aliasing, transparency, gradients, glow. They draw straight into
 * fm1-base's canvas (cv_px, from gfx.c in the same unity build) and mix each pixel with what is there. Part of
 * app.c (included). */

typedef struct { float r, g, b; } col3;

static inline col3 c3(uint16_t c)
{
    col3 o;
    o.r = (float)((c >> 11) & 31u) * (255.0f / 31.0f);
    o.g = (float)((c >> 5) & 63u) * (255.0f / 63.0f);
    o.b = (float)(c & 31u) * (255.0f / 31.0f);
    return o;
}
static inline uint16_t c565(col3 c)
{
    int r = (int)(c.r + 0.5f), g = (int)(c.g + 0.5f), b = (int)(c.b + 0.5f);
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return RGB(r, g, b);
}
static inline col3 mix3(col3 a, col3 b, float t)
{
    a.r += (b.r - a.r) * t;
    a.g += (b.g - a.g) * t;
    a.b += (b.b - a.b) * t;
    return a;
}
static inline col3 add3(col3 a, col3 b, float t)   /* Licht: aufaddiert (Leuchten) */
{
    a.r += b.r * t;
    a.g += b.g * t;
    a.b += b.b * t;
    return a;
}
static inline col3 rgb3(float r, float g, float b)
{
    col3 o;
    o.r = r;
    o.g = g;
    o.b = b;
    return o;
}

/* ein Pixel der Leinwand lesen / schreiben (in Leinwand-Koordinaten); außerhalb: nichts */
static inline int px_ok(int32_t x, int32_t y)
{
    y += cv_oy;
    return (uint32_t)x < cv_w && y >= cv_y0 && y < cv_y1;
}
static inline col3 px_get(int32_t x, int32_t y)
{
    return c3(swap16(cv_px[(uint32_t)(y + cv_oy) * cv_w + (uint32_t)x]));
}
static inline void px_put(int32_t x, int32_t y, col3 c)
{
    cv_px[(uint32_t)(y + cv_oy) * cv_w + (uint32_t)x] = swap16(c565(c));
}
static inline void px_blend(int32_t x, int32_t y, col3 c, float a)
{
    if (a <= 0.004f || !px_ok(x, y))
        return;
    px_put(x, y, a >= 0.996f ? c : mix3(px_get(x, y), c, a));
}
static inline void px_glow(int32_t x, int32_t y, col3 c, float a)
{
    if (a <= 0.004f || !px_ok(x, y))
        return;
    px_put(x, y, add3(px_get(x, y), c, a));
}

static inline float sat01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

/* der Winkel im Uhrzeigersinn ab oben, in Grad (-180 .. 180) */
static inline float ang_deg(float dx, float dy)
{
    float y = dx, x = -dy, ax = x < 0.0f ? -x : x, ay = y < 0.0f ? -y : y, a, s, r;
    if (ax < 1e-6f && ay < 1e-6f)
        return 0.0f;
    a = ax < ay ? ax / ay : ay / ax;
    s = a * a;
    r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax)
        r = 1.5707963f - r;
    if (x < 0.0f)
        r = 3.14159265f - r;
    if (y < 0.0f)
        r = -r;
    return r * 57.29578f;
}

/* x modulo m, immer positiv */
static inline float fmod_pos(float x, float m)
{
    float q = x / m;
    q = x - (float)(int32_t)q * m;
    return q < 0.0f ? q + m : q;
}

/* eine kantengeglättete Linie (Xiaolin Wu), Deckkraft a */
static void aa_line(float x0, float y0, float x1, float y1, col3 c, float a)
{
    float dx = x1 - x0, dy = y1 - y0, adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy, t, n, k;
    int i;
    if (adx < 0.01f && ady < 0.01f) {
        px_blend((int32_t)x0, (int32_t)y0, c, a);
        return;
    }
    if (adx >= ady) {
        if (x1 < x0) {
            t = x0; x0 = x1; x1 = t;
            t = y0; y0 = y1; y1 = t;
        }
        k = dy / dx;
        n = x1 - x0;
        for (i = 0; i <= (int)(n + 0.5f); i++) {
            float x = x0 + (float)i, y = y0 + k * (float)i, f;
            int32_t yi = (int32_t)(y < 0 ? y - 1.0f : y);
            f = y - (float)yi;
            px_blend((int32_t)(x + 0.5f), yi, c, a * (1.0f - f));
            px_blend((int32_t)(x + 0.5f), yi + 1, c, a * f);
        }
    } else {
        if (y1 < y0) {
            t = x0; x0 = x1; x1 = t;
            t = y0; y0 = y1; y1 = t;
        }
        k = dx / dy;
        n = y1 - y0;
        for (i = 0; i <= (int)(n + 0.5f); i++) {
            float y = y0 + (float)i, x = x0 + k * (float)i, f;
            int32_t xi = (int32_t)(x < 0 ? x - 1.0f : x);
            f = x - (float)xi;
            px_blend(xi, (int32_t)(y + 0.5f), c, a * (1.0f - f));
            px_blend(xi + 1, (int32_t)(y + 0.5f), c, a * f);
        }
    }
}

/* eine leuchtende Linie: ein weicher Schein, darüber der Kern */
static void glow_line(float x0, float y0, float x1, float y1, col3 c, float a)
{
    aa_line(x0, y0 - 1.0f, x1, y1 - 1.0f, c, a * 0.22f);
    aa_line(x0, y0 + 1.0f, x1, y1 + 1.0f, c, a * 0.22f);
    aa_line(x0 - 1.0f, y0, x1 - 1.0f, y1, c, a * 0.12f);
    aa_line(x0 + 1.0f, y0, x1 + 1.0f, y1, c, a * 0.12f);
    aa_line(x0, y0, x1, y1, c, a);
}

/* ein weicher Lichtfleck (Bloom), Radius r */
static void bloom(float cx, float cy, float r, col3 c, float a)
{
    int32_t x, y, x0 = (int32_t)(cx - r - 1), x1 = (int32_t)(cx + r + 1), y0 = (int32_t)(cy - r - 1), y1 = (int32_t)(cy + r + 1);
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy, d2 = (dx * dx + dy * dy) / (r * r);
            if (d2 < 1.0f)
                px_glow(x, y, c, a * (1.0f - d2) * (1.0f - d2));
        }
}

/* eine kantengeglättete Scheibe */
static void aa_disc(float cx, float cy, float r, col3 c, float a)
{
    int32_t x, y;
    for (y = (int32_t)(cy - r - 1); y <= (int32_t)(cy + r + 1); y++)
        for (x = (int32_t)(cx - r - 1); x <= (int32_t)(cx + r + 1); x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy, d = fm_sqrtf(dx * dx + dy * dy);
            px_blend(x, y, c, a * sat01(r - d + 0.5f));
        }
}

/* ein Ringbogen von a0 bis a1 (Grad im Uhrzeigersinn ab oben), Mitte rm, Breite w, mit runden Enden;
 * glow > 0: ein Lichtschein außen herum */
static void ring_arc(float cx, float cy, float rm, float w, float a0, float a1, col3 c, float a, float glow)
{
    float ro = rm + w * 0.5f + glow + 1.0f, hw = w * 0.5f, mid = (a0 + a1) * 0.5f, half = (a1 - a0) * 0.5f;
    int32_t x, y;
    if (half < 0.0f)
        half = -half;
    for (y = (int32_t)(cy - ro); y <= (int32_t)(cy + ro); y++)
        for (x = (int32_t)(cx - ro); x <= (int32_t)(cx + ro); x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy, d = fm_sqrtf(dx * dx + dy * dy);
            float rd = d - rm, ard = rd < 0 ? -rd : rd, an, da, along, cov;
            if (ard > hw + glow + 1.0f)
                continue;
            an = ang_deg(dx, dy);
            da = an - mid;
            if (da > 180.0f)
                da -= 360.0f;
            if (da < -180.0f)
                da += 360.0f;
            da = da < 0 ? -da : da;
            along = (da - half) * 0.017453f * d;   /* wie weit hinter dem Ende (in Pixeln) */
            if (along > 0.0f) {                    /* runde Enden */
                float e = fm_sqrtf(along * along + rd * rd);
                cov = sat01(hw - e + 0.5f);
                if (glow > 0.0f && e > hw) {
                    float g = 1.0f - sat01((e - hw) / glow);
                    px_glow(x, y, c, a * 0.35f * g * g);
                }
            } else {
                cov = sat01(hw - ard + 0.5f);
                if (glow > 0.0f && ard > hw) {
                    float g = 1.0f - sat01((ard - hw) / glow);
                    px_glow(x, y, c, a * 0.35f * g * g);
                }
            }
            if (cov > 0.0f)
                px_blend(x, y, c, a * cov);
        }
}

/* Text mit einem Leuchten (der Schein zuerst, versetzt und blass) */
static void text_glow(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c, uint16_t halo)
{
    cv_text(x - 1, y, f, s, halo);
    cv_text(x + 1, y, f, s, halo);
    cv_text(x, y - 1, f, s, halo);
    cv_text(x, y + 1, f, s, halo);
    cv_text(x, y, f, s, c);
}
