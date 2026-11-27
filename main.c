// WRONG TURN - a neon fork-in-the-road game made with raylib.
// Controls (keyboard / mouse / touch):
//   keyboard: LEFT/RIGHT (or A/D) step the selection; in the intro they pick the difficulty (left = easy, right = hard);
//             SPACE/ENTER retries; M mutes; F11 toggles fullscreen.
//   mouse:    move left/right to select, click a path (or a side) to select it; click retries.
//   touch:    swipe left/right to step the selection, tap a path to select it; tap retries.
// Platforms with no keyboard (Android, iOS, or -DWT_NO_KEYBOARD) compile the keyboard handling out and show no keyboard hints;
// native desktop builds show keyboard hints from the start; a web page on a phone (coarse pointer) starts without them
// and shows them once a key is pressed.
#include "raylib.h"
#include "raymath.h"
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#ifdef PLATFORM_WEB
#include <emscripten/emscripten.h>
#endif
#if defined(PLATFORM_ANDROID) || defined(PLATFORM_IOS)
#define WT_NO_KEYBOARD 1
#endif

#define PATH_W 110.0f
#define MAXL 4000
#define PAL_N 6
#define MAXP 300
#define START_LEN 300.0f
#ifndef WIN_SCORE
#define WIN_SCORE 999 // reaching this score ends the game: the road ends in a dead end and you win
#endif

static const Color PAL[PAL_N] = {
    {0, 255, 255, 255}, {255, 40, 200, 255}, {120, 255, 40, 255},
    {255, 150, 0, 255}, {150, 90, 255, 255}, {255, 240, 40, 255}};

// A fork has a real branch N (direction `side`, -1 left / 0 middle / +1 right) and one or two dead ends (D, E).
// `end`: the last stretch of the road, a dead end with no branches (the player wins on reaching it).
// `merge`: the two branches split and join again at N, both lead on, but the middle of each blinks open/closed
// (the left branch is D1/D2, the right one E1/E2); an orb travels between the two gaps and fills the one it is in.
typedef struct { Vector2 P, F, N, D1, D2, E1, E2; int side, dirD, three, merge, end; float S, gapPhase, orbF, orbVel, orbHue; } Level;
typedef struct { Vector2 pos, vel; float life, max; Color col; float size; } Particle;
typedef enum { ST_INTRO, ST_ZOOM, ST_PLAY, ST_DEAD, ST_WON } State;
// Which segment of a fork the player is on. The values double as segment types in the per-segment arrays:
// (see SegState; in a merge fork the kinds 1..6 walk along the left (1-3) or right (4-6) branch)
typedef enum { K_APPROACH, K_TRUE, K_D1, K_D2, K_E1, K_E2, K_X6 } Kind;

static Level L[MAXL];
// Per-segment state. A fork has up to 7 segments (type t). Normal fork: 0 approach, 1 real branch, 2/3 dead branch + tip,
// 4/5 second dead branch + tip (3-way forks). Merge fork: 0 approach, 1-3 left branch (out, middle, back in), 4-6 right branch.
typedef struct {
    float fill;                 // yellow fill fraction, 0..1, from the segment's start
    float burn;                 // 0 = intact ... 1 = gone, measured from the segment's start (its root)
    float len;                  // length, cached when the level is generated
    unsigned char target;       // should this segment be yellow?
    unsigned char burning;      // is it burning away?
    unsigned char alt;          // was it an alternative that was not taken (drawn red)?
} SegState;
#define NSEG 7
static SegState S[MAXL][NSEG];
static int gen;
static Particle parts[MAXP];
// Burn state ("fuse"): once a path can no longer be selected it burns away from its root.
static int introSel, deathBurn;
#define BURN_SPEED 1000.0f
static const Color FYEL = {255, 240, 40, 255}; // the fuse yellow (the red and yellow fills themselves live in the path shader)
static const Color FREDC = {255, 30, 50, 255}; // the red of the branches that are not selected (orbs follow the same colours)
static State st;
static Kind kind;
static Vector2 pos, to;
static int lv, choice, best[2], hard, startSide = 1, lastSide = 1;
static float runT, speed, zoomT, deadT, winT, flash, shake, tnow;
// Three typefaces: Tilt Neon for the UI text, Neoneon for the title, Beon for the score numbers.
// bold: sub-pixel thickening of thin strokes, as a fraction of the font size (0 = none).
typedef struct { Font f; float bold; } Face;
static Face faceUI, faceTitle, faceNum;
static Camera2D cam;
// Desktop uses GLSL 330; the web build (WebGL1 / GLSL ES 1.00) needs a different header.
#ifdef PLATFORM_WEB
#define GLSL_HEAD "#version 100\n#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n#define finalColor gl_FragColor\n"
#else
#define GLSL_HEAD "#version 330\nout vec4 finalColor;\n"
#endif

static int kbHint = 1; // show keyboard hints? (a physical keyboard is present / was used)
// Zoom-out to prepare for the first merge forks: the zoom factor eases out just before the first one appears,
// and eases back to normal as merge forks are passed (or, failing that, as forks go by).
static int firstMerge = -1, mergesPassed;
static float mergeZoom = 1;
static float scorePop;  // score "pop" animation, 1 -> 0
static float uiS = 1; // UI scale (text, arrows) for the current screen size
static Shader fogShader;
static int locCenter, locRadius, locStrength;
// Fog of war: darkness grows with the distance from the player and is fully dark far away, over the whole screen.
static const char *fogFs =
    GLSL_HEAD
    "uniform vec2 center;\nuniform float radius;\nuniform float strength;\n"
    "void main() {\n"
    "    float d = clamp(length(gl_FragCoord.xy - center) / radius, 0.0, 1.0);\n"
    "    float n = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);\n"
    "    finalColor = vec4(0.0, 0.0, 0.0, clamp(strength * pow(d, 1.4) + (n - 0.5) / 128.0, 0.0, 1.0));\n"
    "}\n";

static float frand(float a, float b) { return a + (b - a) * (float)GetRandomValue(0, 10000) / 10000.0f; }
static float ease(float t) { return t < 0.5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2; }
// Everything that differs between easy (0) and hard (1), indexed by `hard`.
typedef struct {
    const char *name; int palIdx;                      // label and its colour (index into PAL)
    float spd0, spdStep, spdMax;                       // run speed: spd0 + spdStep * fork, capped
    float gapMin, gapStep;                             // straight stretch before a fork: max(gapMin, 320 - gapStep * fork) + 0..40
    int three0; float threeP0, threeStep, threeMax;    // 3-way forks: from fork three0, chance threeP0 + threeStep * (fork - three0), capped
    float deadLo, deadHi;                              // dead-end length, as a fraction of the real branch (2-way forks)
    float extra0, extraStep, extraCap, extraRand;      // extra length of a dead end's tip
    float zStart, zMax, zSecs;                         // gameplay zoom: starts zoomed out, eases in over zSecs
    float fogMax, fogStep, fogMin;                     // fog radius (px at 900 px height): clamp(fogMax - fogStep * fork)
    int merge0; float mergeP0, mergeStep, mergeMax;    // merge forks (with the orb): from fork merge0, chance mergeP0 + mergeStep * (fork - merge0), capped at mergeMax
    float startPitch;                                  // pitch of the start whoosh
} Diff;
static const Diff DIFF[2] = {
    {"EASY", 0, 300, 10, 520, 240, 3, 8, 0.2f, 0.03f, 0.5f, 0.4f, 0.6f, 40, 4, 60, 30, 0.45f, 0.8f, 90, 1500, 20, 900, 50, 0.06f, 0.012f, 0.4f, 0.9f},
    {"HARD", 1, 380, 20, 780, 170, 8, 4, 0.3f, 0.04f, 0.7f, 0.6f, 0.85f, 60, 10, 160, 50, 0.5f, 0.95f, 60, 1100, 40, 520, 50, 0.05f, 0.01f, 0.35f, 1.2f},
};
static float baseSpeed(int l) { const Diff *d = &DIFF[hard]; return fminf(d->spd0 + d->spdStep * l, d->spdMax); }


// ---------------------------------------------------------------- audio (synthesized, stereo, no asset files)
// Raylib's mixer is stereo, so the spatial work is done with stereo imaging: effects are pre-panned left/centre/right
// (the side you lean towards), have a stereo reverb tail, and a streamed bed follows the game (drone, lean pad, fuse).
#define SR 44100
#define CHUNK 2048
typedef struct { Sound v[3]; } Sfx; // pre-panned variants: [0] left, [1] centre, [2] right
static Sfx sStart, sPick, sWrong, sCrash, sWin;
static AudioStream bed;
static int audioOK, muted, bedOn;
static int inBackground; // window not focused / hidden: the frame rate drops, so the sound is muted instead of stuttering
// live controls for the streamed bed, set by the game every frame
static float aud_drone, aud_speed = 380, aud_pan, aud_lean, aud_burn, aud_burnPan;

static unsigned rng_state = 12345u;
static float nz(void) { rng_state = rng_state * 1664525u + 1013904223u; return ((rng_state >> 8) & 0xFFFF) / 32768.0f - 1.0f; }

static void alloc_lr(int n, float **l, float **r) {
    *l = (float *)MemAlloc(n * sizeof(float));
    *r = (float *)MemAlloc(n * sizeof(float));
}
// constant-power pan: pan -1 (left) .. +1 (right), unity gain at the centre
static void pan_gains(float pan, float *gl, float *gr) {
    float th = (pan + 1) * PI / 4;
    *gl = cosf(th) * 1.41f; *gr = sinf(th) * 1.41f;
}

// small stereo reverb: parallel combs with different delays per ear, fed by the mono sum
static void reverb(float *l, float *r, int n, float wet) {
    static const float dL[4] = {0.0297f, 0.0371f, 0.0411f, 0.0437f}, dR[4] = {0.0311f, 0.0389f, 0.0423f, 0.0460f};
    float *wl = (float *)MemAlloc(n * sizeof(float)), *wr = (float *)MemAlloc(n * sizeof(float)), *c = (float *)MemAlloc(n * sizeof(float));
    for (int ch = 0; ch < 2; ch++) {
        float *w = ch ? wr : wl;
        for (int k = 0; k < 4; k++) {
            int d = (int)((ch ? dR[k] : dL[k]) * SR);
            for (int i = 0; i < n; i++) {
                c[i] = 0.5f * (l[i] + r[i]) + (i >= d ? 0.74f * c[i - d] : 0.0f);
                w[i] += c[i] * 0.22f;
            }
        }
    }
    for (int i = 0; i < n; i++) { l[i] += wet * wl[i]; r[i] += wet * wr[i]; }
    MemFree(wl); MemFree(wr); MemFree(c);
}

static Sfx make_sfx(float *l, float *r, int n, float peak, int panned) { // panned: left/right variants too
    float mx = 1e-6f;
    for (int i = 0; i < n; i++) mx = fmaxf(mx, fmaxf(fabsf(l[i]), fabsf(r[i])));
    float g = peak / mx;
    static const float gl[3] = {1.0f, 0.75f, 0.35f}, gr[3] = {0.35f, 0.75f, 1.0f};
    Sfx s;
    for (int v = 0; v < 3; v++) {
        if (!panned && v != 1) continue;
        short *d = (short *)MemAlloc(n * 2 * sizeof(short));
        for (int i = 0; i < n; i++) {
            d[2 * i] = (short)(Clamp(l[i] * g * gl[v], -1, 1) * 32000);
            d[2 * i + 1] = (short)(Clamp(r[i] * g * gr[v], -1, 1) * 32000);
        }
        Wave w = {(unsigned)n, SR, 16, 2, d};
        s.v[v] = LoadSoundFromWave(w);
        MemFree(d);
    }
    MemFree(l); MemFree(r);
    return s;
}

static Sfx gen_start(void) { // rising whoosh that sweeps from the left ear to the right
    int n = (int)(1.5f * SR);
    float *l, *r;
    alloc_lr(n, &l, &r);
    double ph = 0;
    float lp = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR, u = t / 0.9f;
        if (u > 1) break;
        ph += 2 * PI * (110.0 * pow(7.0, u)) / SR;
        lp += (nz() - lp) * (0.02f + 0.3f * u);
        float s = ((float)sin(ph) + 0.35f * (float)sin(2 * ph)) * 0.5f + lp * 0.9f;
        s *= sinf(u * PI);
        l[i] = s * cosf(u * PI / 2); r[i] = s * sinf(u * PI / 2);
    }
    reverb(l, r, n, 0.35f);
    return make_sfx(l, r, n, 0.5f, 0);
}

static Sfx gen_pick(void) { // soft tick
    int n = (int)(0.45f * SR);
    float *l, *r;
    alloc_lr(n, &l, &r);
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float s = (sinf(2 * PI * 700 * t) + 0.2f * sinf(2 * PI * 1400 * t)) * expf(-t * 40) * fminf(1, t * 300);
        l[i] = r[i] = s;
    }
    reverb(l, r, n, 0.3f);
    return make_sfx(l, r, n, 0.35f, 1);
}

static Sfx gen_wrong(void) { // falling, slightly detuned buzz (wide: each ear has its own pitch)
    int n = (int)(1.0f * SR);
    float *l, *r;
    alloc_lr(n, &l, &r);
    double pl = 0, pr = 0;
    float yl = 0, yr = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR, u = t / 0.5f;
        if (u > 1) break;
        float f = 120.0f * powf(0.4f, u);
        pl += 2 * PI * f / SR; pr += 2 * PI * f * 1.025 / SR;
        float sl = 2 * (float)fmod(pl / (2 * PI), 1.0) - 1, sr = 2 * (float)fmod(pr / (2 * PI), 1.0) - 1;
        yl += (sl - yl) * 0.05f; yr += (sr - yr) * 0.05f;
        float env = expf(-t * 5), burst = expf(-t * 30) * 0.08f;
        l[i] = yl * env * 2.5f + nz() * burst; r[i] = yr * env * 2.5f + nz() * burst;
    }
    reverb(l, r, n, 0.25f);
    return make_sfx(l, r, n, 0.5f, 1);
}

static Sfx gen_crash(void) { // wide noise burst + sub drop
    int n = (int)(2.0f * SR);
    float *l, *r;
    alloc_lr(n, &l, &r);
    double ph = 0;
    float lpl = 0, lpr = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        if (t > 1.2f) break;
        float a = 0.18f * expf(-t * 3) + 0.015f;
        lpl += (nz() - lpl) * a; lpr += (nz() - lpr) * a;
        ph += 2 * PI * (70.0 * exp(-t * 1.5)) / SR;
        float sub = (float)sin(ph) * expf(-t * 3) * 0.6f, click = expf(-t * 60);
        float env = expf(-t * 2.8f);
        l[i] = lpl * 3 * env + sub + nz() * click * 0.25f; r[i] = lpr * 3 * env + sub + nz() * click * 0.25f;
    }
    reverb(l, r, n, 0.5f);
    return make_sfx(l, r, n, 0.65f, 0);
}

#define DT_N 4096
static float droneTab[DT_N + 1]; // one 4*PI period of sin(p) + 0.5 sin(1.5p) + 0.3 sin(2p), for table lookup

// The effects are synthesized one per frame right after startup (not all before the first frame), so the window
// appears immediately; an effect that is not built yet is simply skipped.
static Sfx gen_win(void) { // calm victory: a slow rising major arpeggio of soft bells that rings out
    int n = (int)(5.0f * SR);
    float *l, *r;
    alloc_lr(n, &l, &r);
    static const float fq[5] = {261.63f, 329.63f, 392.0f, 523.25f, 659.25f}; // C4 E4 G4 C5 E5
    static const float t0[5] = {0.0f, 0.4f, 0.8f, 1.25f, 1.75f}, pan[5] = {-0.4f, 0.3f, -0.2f, 0.45f, 0.0f};
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        for (int k = 0; k < 5; k++) {
            float tt = t - t0[k];
            if (tt < 0) continue;
            float env = (1 - expf(-tt * 14)) * expf(-tt * 0.85f); // soft attack, long calm decay
            float v = sinf(2 * PI * fq[k] * tt) + 0.25f * sinf(4 * PI * fq[k] * tt) * expf(-tt * 1.5f) + 0.08f * sinf(6 * PI * fq[k] * tt) * expf(-tt * 2.5f);
            float gl, gr;
            pan_gains(pan[k], &gl, &gr);
            l[i] += v * env * gl * 0.35f; r[i] += v * env * gr * 0.35f;
        }
    }
    reverb(l, r, n, 0.55f);
    return make_sfx(l, r, n, 0.5f, 0);
}

static void audio_warm(void) {
    static int next;
    if (!audioOK) return;
    switch (next) {
    case 0: sPick = gen_pick(); break;
    case 1: sWrong = gen_wrong(); break;
    case 2: sStart = gen_start(); break;
    case 3: sCrash = gen_crash(); break;
    case 4: sWin = gen_win(); break;
    default: return;
    }
    next++;
}

static void audio_init(void) {
    InitAudioDevice();
    audioOK = IsAudioDeviceReady();
    if (!audioOK) return;
    for (int i = 0; i <= DT_N; i++) {
        double p = i * 4 * PI / DT_N;
        droneTab[i] = (float)(sin(p) + 0.5 * sin(1.5 * p) + 0.3 * sin(2 * p));
    }
    SetAudioStreamBufferSizeDefault(CHUNK);
    bed = LoadAudioStream(SR, 16, 2);
    SetAudioStreamVolume(bed, 0.6f);
}

// dir: -1 left, 0 centre, +1 right
static void sfxp(Sfx s, int dir, float pitch) {
    if (!audioOK || muted || inBackground || !s.v[1].stream.buffer) return; // not built yet
    Sound v = s.v[dir < 0 ? 0 : (dir > 0 ? 2 : 1)];
    SetSoundPitch(v, pitch);
    PlaySound(v);
}

// The streamed bed, synthesized live so it can follow the game:
//  - drone: stereo, the two ears slightly detuned (slow beating = width); pitch rises with speed
//  - lean pad: a tone near a fork that sits on the side you currently lean to (and pulses faster as the fork nears)
//  - fuse: hiss + crackles, panned towards where paths are burning
static float drone_wave(double ph) { // ph in [0, 4*PI)
    double x = ph * (DT_N / (4 * PI));
    int i = (int)x;
    float f = (float)(x - i);
    return droneTab[i] + (droneTab[i + 1] - droneTab[i]) * f;
}

#define BLK 32 // control rate: parameters (pan, pitch, LFOs) update once per block, audio runs per sample
static void bed_fill(short *out, int frames) {
    static double ph[4], tcnt;
    static float sDrone, sPan, sLean, sBurn, sBurnPan, sSpd = 380, lps[2];
    for (int b0 = 0; b0 < frames; b0 += BLK) {
        int nb = frames - b0 < BLK ? frames - b0 : BLK;
        float k = (float)nb; // the per-sample smoothing constants below, scaled to the block
        sDrone += (aud_drone - sDrone) * 0.0003f * k;
        sPan += (aud_pan - sPan) * 0.0012f * k;
        sLean += (aud_lean - sLean) * 0.0008f * k;
        sBurn += (aud_burn - sBurn) * 0.0008f * k;
        sBurnPan += (aud_burnPan - sBurnPan) * 0.0008f * k;
        sSpd += (aud_speed - sSpd) * 0.0003f * k;
        tcnt += (double)nb / SR;
        double wl = 2 * PI * 55.0 * (0.8 + 0.8 * sSpd / 780.0) / SR, wr = wl * 1.004;
        double wpad = 2 * PI * 220.0 * (1.0 + 0.05 * sPan) / SR;
        float lfo = 0.8f + 0.2f * (float)sin(2 * PI * 0.5 * tcnt);
        float pulse = 0.65f + 0.35f * (float)sin(2 * PI * (1.5 + 4.0 * sLean) * tcnt);
        float dgain = 0.13f * lfo * sDrone, pgain = pulse * sLean * 0.08f, bgain = 0.035f * sBurn;
        float padL, padR, burnL, burnR;
        pan_gains(sPan, &padL, &padR);
        pan_gains(sBurnPan, &burnL, &burnR);
        for (int i = b0; i < b0 + nb; i++) {
            ph[0] += wl; if (ph[0] >= 4 * PI) ph[0] -= 4 * PI;
            ph[1] += wr; if (ph[1] >= 4 * PI) ph[1] -= 4 * PI;
            ph[2] += wpad; ph[3] += wpad * 1.5;
            float pad = (float)(0.7 * sin(ph[2]) + 0.3 * sin(ph[3])) * pgain;
            // static: soft, band-limited noise at a low level (no clicks, no harsh highs)
            lps[0] += (nz() - lps[0]) * 0.18f; lps[1] += (nz() - lps[1]) * 0.18f;
            out[2 * i] = (short)(Clamp(drone_wave(ph[0]) * dgain + pad * padL + lps[0] * bgain * burnL, -1, 1) * 32000);
            out[2 * i + 1] = (short)(Clamp(drone_wave(ph[1]) * dgain + pad * padR + lps[1] * bgain * burnR, -1, 1) * 32000);
        }
    }
}

static void bed_update(void) {
    if (!audioOK) return;
    if (muted || inBackground) { if (bedOn) { StopAudioStream(bed); bedOn = 0; } return; }
    if (!bedOn) { PlayAudioStream(bed); bedOn = 1; }
    static short tmp[CHUNK * 2];
    while (IsAudioStreamProcessed(bed)) { bed_fill(tmp, CHUNK); UpdateAudioStream(bed, tmp, CHUNK); }
}

// ---------------------------------------------------------------- level gen
static void make_dead(Level *l, int dir, float a, float T, float maxDepth, int i, Vector2 *b1, Vector2 *b2) {
    if (!dir) a = 0; // the middle branch goes straight up
    const Diff *d = &DIFF[hard];
    float Td = T * (l->three ? frand(0.65f, 0.9f) : frand(d->deadLo, d->deadHi));
    float depth = fminf(cosf(a) * Td, maxDepth * 0.75f);
    Td = depth / cosf(a);
    *b1 = (Vector2){l->F.x + dir * sinf(a) * Td, l->F.y - depth};
    float extra = d->extra0 + fminf(i * d->extraStep, d->extraCap) + frand(0, d->extraRand);
    float d2 = fminf(depth + extra, maxDepth);
    *b2 = (Vector2){b1->x, l->F.y - d2};
}

// the levels around the player that are simulated and drawn
static int win_hi(void) { return lv + 8 < gen ? lv + 8 : gen - 1; }
static int win_lo(int back) { return lv > back ? lv - back : 0; }
// does segment (j, t) exist? (type 6 and the middle types differ between fork shapes)
static int seg_exists(int j, int t) { return L[j].end ? t == 0 : L[j].merge ? 1 : (t < 4 || (t < 6 && L[j].three)); }
static void seg_geo(int j, int t, Vector2 *a, Vector2 *b);

static int put_seg(float *arr, int *n, int cap, Vector2 a, Vector2 b);

// ---- the orb and the gaps. The middle segment of each branch of a merge fork (types 2 and 5) has a gap, and an orb
// travels between the two gaps: the gap the orb sits in is closed (filled), the other is open. The orb dwells in a
// gap for a while, then glides across to the other one, so the open/closed toggling is exactly the orb's movement.
// While the player crosses a gap the orb waits in it.
#define GAP_PERIOD 3.0f   // one full round trip of the orb
#define ORB_DWELL 0.4f   // fraction of the cycle spent in each gap; the rest is travelling (2 * (dwell + travel) = 1)
#define ORB_TRAVEL 0.1f
#define GAP_A 0.2f // the gap spans this part of the middle segment, as fractions of its length
#define GAP_B 0.8f
static int has_gap(int j, int t) { return L[j].merge && (t == 2 || t == 5); }
static void gap_tip(int j, int t, Vector2 *A, Vector2 *u, float *len) { // start, unit direction and length of the middle segment
    Vector2 B = t == 2 ? L[j].D2 : L[j].E2;
    *A = t == 2 ? L[j].D1 : L[j].E1;
    Vector2 d = Vector2Subtract(B, *A);
    *len = fmaxf(Vector2Length(d), 1.0f);
    *u = Vector2Scale(d, 1.0f / *len);
}
// is the point inside the segment's gap area (open or not)?
static int in_gap_area(int j, int t, Vector2 p) {
    Vector2 A, u; float len;
    gap_tip(j, t, &A, &u, &len);
    float tp = Vector2DotProduct(Vector2Subtract(p, A), u);
    return tp >= GAP_A * len && tp <= GAP_B * len;
}
static float ease3(float x) { return x * x * (3 - 2 * x); }
// where the orb is at time tm: 0 = in the left branch's gap, 1 = in the right branch's gap, smooth in between
static float orb_cycle_pos(int j, float tm) {
    float u = fmodf(tm / GAP_PERIOD + L[j].gapPhase, 1.0f);
    if (u < ORB_DWELL) return 0;
    if (u < ORB_DWELL + ORB_TRAVEL) return ease3((u - ORB_DWELL) / ORB_TRAVEL);
    if (u < 2 * ORB_DWELL + ORB_TRAVEL) return 1;
    return 1 - ease3((u - 2 * ORB_DWELL - ORB_TRAVEL) / ORB_TRAVEL);
}
// the gap the player is crossing right now (2 or 5), or 0
static int crossing_gap(int j) {
    int t = (int)kind;
    return (st == ST_PLAY || st == ST_ZOOM) && j == lv && has_gap(j, t) && in_gap_area(j, t, pos) ? t : 0;
}
// is branch `side` (0 left, 1 right) of merge fork j the lit (yellow) one? After the player commits it is the branch taken.
static int branch_lit(int j, int side) {
    int committed = S[j][1].alt || S[j][4].alt; // the branch not taken is marked as an alternative
    if (committed) return side == 0 ? !S[j][1].alt : !S[j][4].alt;
    return S[j][side == 0 ? 2 : 5].target;
}
static Color mix_color(Color a, Color b, float f) {
    return (Color){(unsigned char)Lerp(a.r, b.r, f), (unsigned char)Lerp(a.g, b.g, f), (unsigned char)Lerp(a.b, b.b, f), 255};
}

// the orb moves towards where it should be; it waits in the gap the player is crossing
static void update_orbs(float dt) {
    for (int j = win_lo(2); j <= win_hi(); j++) {
        if (!L[j].merge) continue;
        int c = crossing_gap(j);
        float target = c == 2 ? 0.0f : (c == 5 ? 1.0f : orb_cycle_pos(j, tnow));
        float before = L[j].orbF, step = 12.0f * dt; // fast enough to rush to the player's gap
        L[j].orbF = before + Clamp(target - before, -step, step);
        L[j].orbVel = dt > 0 ? (L[j].orbF - before) / dt : 0;
        // colour: red or yellow, gradually, according to the branch it is heading to (or sits in, when it is not moving)
        int dest = fabsf(L[j].orbVel) > 0.3f ? L[j].orbVel > 0 : L[j].orbF > 0.5f;
        float hue = branch_lit(j, dest) ? 1.0f : 0.0f, hstep = 9.0f * dt;
        L[j].orbHue += Clamp(hue - L[j].orbHue, -hstep, hstep);
    }
}
// A gap is drawn closed only while the orb really sits in it. (So the orb is visible exactly while a gap is open.)
static int gap_open(int j, int t) { return t == 2 ? L[j].orbF > 0.12f : L[j].orbF < 0.88f; }
// Stepping onto a gap only kills if the orb is far from it: when the orb is within reach (it is on its way there or
// has just left) it rushes in and fills the gap for the player. Leaving one gap and arriving at the other overlap in
// the middle of the trip, so at any moment the player can safely cross at least one of the two.
#define ORB_REACH 0.67f
static int gap_deadly(int j, int t) { return t == 2 ? L[j].orbF > ORB_REACH : L[j].orbF < 1.0f - ORB_REACH; }
// put_seg for segment (j, t); where the gap is open the middle is left out (up to two pieces are emitted)
static int put_seg_j(float *arr, int *n, int cap, int j, int t, Vector2 a, Vector2 b) {
    if (!has_gap(j, t) || !gap_open(j, t)) return put_seg(arr, n, cap, a, b);
    Vector2 A, u; float len;
    gap_tip(j, t, &A, &u, &len);
    float ta = Vector2DotProduct(Vector2Subtract(a, A), u), tb = Vector2DotProduct(Vector2Subtract(b, A), u);
    float g0 = GAP_A * len, g1 = GAP_B * len, e1 = fminf(tb, g0), s2 = fmaxf(ta, g1);
    int k = 0;
    if (ta < e1) k += put_seg(arr, n, cap, Vector2Add(A, Vector2Scale(u, ta)), Vector2Add(A, Vector2Scale(u, e1)));
    if (s2 < tb) k += put_seg(arr, n, cap, Vector2Add(A, Vector2Scale(u, s2)), Vector2Add(A, Vector2Scale(u, tb)));
    return k;
}

static void build_level(int i) {
    Level *l = &L[i];
    if (i == 0) { l->P = (Vector2){0, 0}; l->F = (Vector2){0, -START_LEN}; }
    else { l->P = L[i - 1].N; l->F = (Vector2){l->P.x, l->P.y - L[i - 1].S}; }
    l->end = 0;
    if (i == WIN_SCORE) { // the end of the road: no more forks, just a long dead end
        l->F = (Vector2){l->P.x, l->P.y - 700};
        l->N = l->D1 = l->D2 = l->E1 = l->E2 = l->F;
        l->side = 1; l->dirD = -1; l->three = l->merge = 0; l->S = 0; l->gapPhase = l->orbF = l->orbVel = l->orbHue = 0;
        l->end = 1;
        return;
    }
    float T = frand(340, 420);
    const Diff *df = &DIFF[hard];
    l->S = fmaxf(df->gapMin, 320.0f - i * df->gapStep) + frand(0, 40);
    l->E1 = l->E2 = l->F;
    l->three = 0; l->merge = 0; l->gapPhase = 0; l->orbF = 0; l->orbVel = 0; l->orbHue = 0;
    if (i == 0) {
        // the intro fork: two identical-looking branches, the chosen one is the real path
        float a = 40 * DEG2RAD;
        l->side = startSide; l->dirD = -startSide; T = 360;
        l->N = (Vector2){l->F.x + l->side * sinf(a) * T, l->F.y - cosf(a) * T};
        l->D1 = l->D2 = (Vector2){l->F.x - l->side * sinf(a) * T, l->F.y - cosf(a) * T};
        return;
    }
    // some forks split and merge again: both branches lead on, but the middle of each blinks open/closed
    if (i >= df->merge0 && frand(0, 1) < fminf(df->mergeP0 + df->mergeStep * (i - df->merge0), df->mergeMax)) {
        l->merge = 1; l->side = 1; l->dirD = -1;
        l->gapPhase = frand(0, 1);
        float ang = frand(35, 45) * DEG2RAD, diag = frand(190, 230), mid = frand(300, 360);
        float dx = sinf(ang) * diag, dy = cosf(ang) * diag;
        l->D1 = (Vector2){l->F.x - dx, l->F.y - dy}; l->D2 = (Vector2){l->D1.x, l->D1.y - mid};
        l->E1 = (Vector2){l->F.x + dx, l->F.y - dy}; l->E2 = (Vector2){l->E1.x, l->E1.y - mid};
        l->N = (Vector2){l->F.x, l->F.y - 2 * dy - mid};
        l->S = 0.7f * fmaxf(df->gapMin, 320.0f - i * df->gapStep) + frand(0, 30); // the fork itself is long
        return;
    }
    // later on, forks get a third (middle) branch: more alternatives, more difficulty
    float pr = i >= df->three0 ? fminf(df->threeP0 + df->threeStep * (i - df->three0), df->threeMax) : 0;
    l->three = frand(0, 1) < pr;
    int dirE = 0;
    float aN, aD, aE = 0;
    if (!l->three) {
        // keep the route from drifting too far sideways
        l->side = l->F.x > 260 ? -1 : (l->F.x < -260 ? 1 : (GetRandomValue(0, 1) ? 1 : -1));
        l->dirD = -l->side;
        aN = frand(32, 46) * DEG2RAD; aD = frand(32, 46) * DEG2RAD;
    } else {
        int allowed[3], n = 0;
        for (int d = -1; d <= 1; d++) {
            if (d == 1 && l->F.x > 260) continue;
            if (d == -1 && l->F.x < -260) continue;
            allowed[n++] = d;
        }
        l->side = allowed[GetRandomValue(0, n - 1)];
        int rest[2], r = 0;
        for (int d = -1; d <= 1; d++) if (d != l->side) rest[r++] = d;
        int sw = GetRandomValue(0, 1);
        l->dirD = rest[sw]; dirE = rest[1 - sw];
        aN = frand(48, 60) * DEG2RAD; aD = frand(48, 60) * DEG2RAD; aE = frand(48, 60) * DEG2RAD;
    }
    l->N = l->side == 0 ? (Vector2){l->F.x, l->F.y - T * 0.8f}
                        : (Vector2){l->F.x + l->side * sinf(aN) * T, l->F.y - cosf(aN) * T};
    // dead ends: with a bend, clamped so they never touch the next fork's area
    float maxDepth = (l->F.y - (l->N.y - l->S)) - 150;
    make_dead(l, l->dirD, aD, T, maxDepth, i, &l->D1, &l->D2);
    if (l->three) make_dead(l, dirE, aE, T, maxDepth, i, &l->E1, &l->E2);
}
static void gen_level(int i) {
    build_level(i);
    for (int t = 0; t < NSEG; t++) {
        Vector2 a, b;
        seg_geo(i, t, &a, &b);
        S[i][t].len = fmaxf(Vector2Distance(a, b), 1.0f);
    }
}
static void ensure_gen(int n) { while (gen <= n && gen <= WIN_SCORE && gen < MAXL) gen_level(gen++); }

// ---------------------------------------------------------------- drawing
// The whole path (neon tubes, red/yellow fills, glow) is rendered in ONE fragment shader from signed
// distances to the segments, so there are no overlapping translucent shapes and no visible circles.
#define MAXPS 56
#define MAXRS 48
#define MAXYS 32
static float uP[MAXPS * 4], uPc[MAXPS * 3], uR[MAXRS * 4], uY[MAXYS * 4];
static int nuP, nuR, nuY;
static Rectangle viewR; // visible world rectangle (+ margin), used to skip off-screen segments
static Shader pathShader;
static int locCamT, locCamOff, locZoom, locScrH, locPulse, locTime, locPw, locFade, locNP, locNR, locNY, locP, locPc, locR, locY;

#define STR_(x) #x
#define STR(x) STR_(x)
static const char *pathFs =
    GLSL_HEAD
    "uniform vec2 camT, camOff;\nuniform float zoom, scrH, pulse, pw, time, pathFade;\n"
    "uniform int nP, nR, nY;\n"
    "uniform vec4 pSeg[" STR(MAXPS) "];\nuniform vec3 pCol[" STR(MAXPS) "];\n"
    "uniform vec4 rSeg[" STR(MAXRS) "];\nuniform vec4 ySeg[" STR(MAXYS) "];\n"
    "float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }\n"
    "vec3 stars(vec2 q, float zm) {\n"                      // sparse twinkling background dust with parallax
    "    vec3 acc = vec3(0.0);\n"
    "    for (int l = 0; l < 2; l++) {\n"
    "        float cell = l == 0 ? 130.0 : 80.0;\n"
    "        vec2 g = q / cell + float(l) * 17.3;\n"
    "        vec2 id = floor(g), f = fract(g);\n"
    "        float h = hash(id);\n"
    "        if (h > 0.9) {\n"
    "            vec2 c = vec2(hash(id + 1.7), hash(id + 5.3));\n"
    "            float d = length(f - c) * cell * zm;\n"
    "            float tw = 0.55 + 0.45 * sin(time * (1.0 + h * 3.0) + h * 40.0);\n"
    "            acc += mix(vec3(0.3, 0.8, 1.0), vec3(1.0, 0.3, 0.9), hash(id + 9.1)) * smoothstep(2.4, 0.0, d) * tw * 0.45;\n"
    "        }\n"
    "    }\n"
    "    return acc;\n"
    "}\n"
    "float sdSeg(vec2 p, vec4 s) {\n"
    "    vec2 pa = p - s.xy, ba = s.zw - s.xy;\n"
    "    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-3), 0.0, 1.0);\n"
    "    return length(pa - ba * h);\n"
    "}\n"
    "void main() {\n"
    "    vec2 sc = vec2(gl_FragCoord.x, scrH - gl_FragCoord.y);\n"
    "    vec2 p = (sc - camOff) / zoom + camT;\n"
    "    float aa = 1.0 / zoom;\n"          // one pixel in world units
    "    vec3 col = vec3(0.0);\n"
    "    float covOut = 0.0;\n"
    "    if (nP > 0) {\n"
    "        float dmin = 1e9;\n"
    "        for (int i = 0; i < " STR(MAXPS) "; i++) { if (i >= nP) break; dmin = min(dmin, sdSeg(p, pSeg[i])); }\n"
    "        float sd = dmin - pw * 0.5;\n"
    "        float sp = sd * zoom;\n"                              // signed distance in pixels
    "        if (sp < 180.0) {\n"                                  // farther away the glow is invisible: skip the colour pass
    "        vec3 acc = vec3(0.0); float wsum = 0.0;\n"           // blend colors smoothly where tubes meet
    "        for (int i = 0; i < " STR(MAXPS) "; i++) {\n"
    "            if (i >= nP) break;\n"
    "            float w = exp(-(sdSeg(p, pSeg[i]) - dmin) * 0.12);\n"
    "            acc += pCol[i] * w; wsum += w;\n"
    "        }\n"
    "        vec3 tc = acc / wsum;\n"
    "        covOut = (1.0 - smoothstep(-aa, aa, sd)) * pathFade;\n"
    "        float covIn = 1.0 - smoothstep(-aa, aa, sd + 5.0);\n"
    "        float ring = covOut - covIn;\n"
    "        float s = max(sp, 0.0);\n"
    "        float glow = 0.5 * exp(-s / 4.0) + 0.22 * exp(-s / 14.0) + 0.1 * exp(-s / 42.0);\n"
    "        float inner = sp < 0.0 ? 0.28 * exp(sp / 12.0) : 0.0;\n"
    "        float lum = ring + (1.0 - covOut) * glow * pulse + covIn * inner;\n"
    "        col = mix(tc, vec3(1.0), 0.3 * ring) * lum * pathFade;\n"
    "        }\n"
    "    }\n"
    "    float fr = 1e9, fyv = 1e9;\n"
    "    for (int i = 0; i < " STR(MAXRS) "; i++) { if (i >= nR) break; fr = min(fr, sdSeg(p, rSeg[i])); }\n"
    "    for (int i = 0; i < " STR(MAXYS) "; i++) { if (i >= nY) break; fyv = min(fyv, sdSeg(p, ySeg[i])); }\n"
    "    float fw = (pw - 16.0) * 0.5;\n"
    "    float dr = fr - fw, dy = fyv - fw;\n"
    "    vec3 red = vec3(1.0, 0.12, 0.2), yel = vec3(1.0, 0.94, 0.16);\n"
    "    float cr = 1.0 - smoothstep(-aa, aa, dr), cy = 1.0 - smoothstep(-aa, aa, dy);\n"
    "    col = mix(col, red * (0.8 + 0.35 * exp(min(dr, 0.0) * zoom / 8.0)), cr);\n"
    "    col = mix(col, yel * (0.8 + 0.35 * exp(min(dy, 0.0) * zoom / 8.0)), cy);\n"
    "    vec2 q = (sc - camOff) / zoom + camT * 0.35;\n"
    "    col += stars(q, zoom) * (1.0 - covOut);\n"
    "    col += (hash(gl_FragCoord.xy + time) - 0.5) / 128.0;\n"   // dither: no gradient banding
    "    finalColor = vec4(max(col, 0.0), 1.0);\n"
    "}\n";

static int seg_visible(Vector2 a, Vector2 b) {
    return !(fmaxf(a.x, b.x) < viewR.x || fminf(a.x, b.x) > viewR.x + viewR.width ||
             fmaxf(a.y, b.y) < viewR.y || fminf(a.y, b.y) > viewR.y + viewR.height);
}
static int put_seg(float *arr, int *n, int cap, Vector2 a, Vector2 b) {
    if (*n >= cap || !seg_visible(a, b)) return 0;
    float *d = arr + *n * 4;
    d[0] = a.x; d[1] = a.y; d[2] = b.x; d[3] = b.y;
    (*n)++;
    return 1;
}

static void seg_geo(int j, int t, Vector2 *a, Vector2 *b);

static void draw_path(void) {
    int lo = win_lo(3), hi = win_hi();
    if (st == ST_INTRO) { lo = 0; hi = 0; }
    nuP = 0;
    for (int j = lo; j <= hi; j++)
        for (int t = 0; t < NSEG; t++) {
            if (!seg_exists(j, t)) continue;
            if (S[j][t].burn >= 1) continue;
            Vector2 a, b;
            seg_geo(j, t, &a, &b);
            a = Vector2Lerp(a, b, S[j][t].burn); // burnt away from the root
            Color c = t == 0 ? PAL[j % PAL_N] : PAL[(j + 1) % PAL_N];
            int k = put_seg_j(uP, &nuP, MAXPS, j, t, a, b); // k pieces (more than one when a gap splits it)
            for (int q = 1; q <= k; q++) {
                float *pc = uPc + (nuP - q) * 3;
                pc[0] = c.r / 255.0f; pc[1] = c.g / 255.0f; pc[2] = c.b / 255.0f;
            }
        }
}



// ---- selection preview: the whole tree ahead is red, the yellow route is drawn over it.
// Yellow is animated per segment: it backtracks fast when the route changes, then fills the new one.
#define FILL_SPEED 2200.0f
#define RETRACT_SPEED 3200.0f

static void seg_geo(int j, int t, Vector2 *a, Vector2 *b) {
    Level *m = &L[j];
    if (m->merge) { // F -> out -> middle -> back in to N, on each side
        switch (t) {
        case 0: *a = m->P; *b = m->F; break;
        case 1: *a = m->F; *b = m->D1; break;
        case 2: *a = m->D1; *b = m->D2; break;
        case 3: *a = m->D2; *b = m->N; break;
        case 4: *a = m->F; *b = m->E1; break;
        case 5: *a = m->E1; *b = m->E2; break;
        default: *a = m->E2; *b = m->N; break;
        }
        return;
    }
    switch (t) {
    case 0: *a = m->P; *b = m->F; break;
    case 1: *a = m->F; *b = m->N; break;
    case 2: *a = m->F; *b = m->D1; break;
    case 3: *a = m->D1; *b = m->D2; break;
    case 4: *a = m->F; *b = m->E1; break;
    case 5: *a = m->E1; *b = m->E2; break;
    default: *a = *b = m->F; break; // (type 6 only exists in merge forks)
    }
}
// the player's current segment type is simply `kind` (see Kind: values double as segment types)
// the fork the player is heading to (or already beyond the current one, after a correct turn)
static int next_fork(void) { return kind == K_APPROACH ? lv : lv + 1; }
// which way the player is leaning at fork j (2-way forks have no middle, so 0 means "keep the last side")
static int eff_sel(int j) { return j == 0 ? L[0].side : (L[j].three ? choice : (choice ? choice : lastSide)); }

static void compute_targets(void) {
    int lo = win_lo(1), hi = win_hi();
    for (int j = lo; j <= hi; j++) for (int t = 0; t < NSEG; t++) S[j][t].target = 0;
    if (st == ST_INTRO) {
        if (introSel) {
            int selN = (introSel > 0) == (L[0].side > 0);
            S[0][0].target = 1; S[0][selN ? 1 : 2].target = 1; S[0][3].target = !selN;
        }
        return;
    }
    if (st == ST_DEAD || st == ST_WON) return;
    int ty = (int)kind, j = lv;
    if (L[lv].merge && ty != 0) { // on a branch of a merge fork: the rest of that branch, then on to the next forks
        for (int t = ty; t <= (ty <= 3 ? 3 : 6); t++) S[lv][t].target = 1;
        j = lv + 1;
    } else {
        if (ty == 2) { S[lv][2].target = S[lv][3].target = 1; return; }
        if (ty == 3) { S[lv][3].target = 1; return; }
        if (ty == 4) { S[lv][4].target = S[lv][5].target = 1; return; }
        if (ty == 5) { S[lv][5].target = 1; return; }
        if (ty == 1) { S[lv][1].target = 1; j = lv + 1; } else S[lv][0].target = 1;
    }
    // the route followed if the player presses nothing: keep the current side at every fork
    for (; j < gen && j <= lv + 7; j++) {
        if (j > lv) S[j][0].target = 1;
        if (L[j].end) break; // the road ends here
        int sel = eff_sel(j);
        if (L[j].merge) { // both branches lead on: the selected side is the one lit
            for (int t = sel < 0 ? 1 : 4; t <= (sel < 0 ? 3 : 6); t++) S[j][t].target = 1;
            continue;
        }
        if (sel == L[j].side) S[j][1].target = 1;
        else if (L[j].three && sel != L[j].dirD) { S[j][4].target = S[j][5].target = 1; break; }
        else { S[j][2].target = S[j][3].target = 1; break; }
    }
}

// the segment a segment grows out of: returns its type and its level in *pj, or -1 for the root of the tree
static int seg_parent(int j, int t, int *pj) {
    if (t == 0) { // the approach grows out of the previous fork's last segment (in a merge fork: the lit branch's)
        *pj = j - 1;
        if (j == 0) return -1;
        return L[j - 1].merge ? (S[j - 1][3].target ? 3 : 6) : 1;
    }
    *pj = j;
    if (L[j].merge) return (t == 1 || t == 4) ? 0 : t - 1;
    return t == 3 ? 2 : (t == 5 ? 4 : 0);
}

// is any segment that grows out of (j, t) still filled? (a segment only retracts once everything beyond it is empty)
static int child_lit(int j, int t, int hi) {
    for (int j2 = j; j2 <= j + 1 && j2 <= hi; j2++)
        for (int t2 = 0; t2 < 6; t2++) {
            int pj, p = seg_parent(j2, t2, &pj);
            if (p == t && pj == j && S[j2][t2].fill > 0) return 1;
        }
    return 0;
}

static float seg_len(int j, int t) { return S[j][t].len; }

static void update_burn(float dt) {
    int lo = win_lo(5), hi = win_hi();
    for (int j = lo; j <= hi; j++)
        for (int t = 0; t < NSEG; t++) {
            if (!S[j][t].burning || S[j][t].burn >= 1) continue;
            // a tip burns after its branch; after a loss the fuse runs through the whole tree, root first
            int pj, p = seg_parent(j, t, &pj);
            if (p >= 0 && (st == ST_DEAD || L[j].merge || t == 3 || t == 5) && S[pj][p].burn < 1) continue;
            S[j][t].burn = fminf(1, S[j][t].burn + (st == ST_DEAD ? 2.5f : 1.0f) * BURN_SPEED * dt / seg_len(j, t));
        }
}

// where is it burning? feeds the fuse sizzle (loudness and left/right position on screen)
static void burn_audio(int W) {
    int lo = win_lo(5), hi = win_hi(), n = 0;
    float sx = 0;
    for (int j = lo; j <= hi; j++)
        for (int t = 0; t < NSEG; t++) {
            if (!S[j][t].burning || S[j][t].burn >= 1) continue;
            Vector2 a, b;
            seg_geo(j, t, &a, &b);
            Vector2 f = Vector2Lerp(a, b, S[j][t].burn);
            sx += (f.x - cam.target.x) * cam.zoom / (W / 2.0f);
            n++;
        }
    aud_burn = (st == ST_DEAD && deathBurn) ? 0.7f : fminf(0.7f, n * 0.2f);
    if (st == ST_PLAY || st == ST_ZOOM) aud_burn = fmaxf(aud_burn, 0.12f); // a faint static under the fuse head
    aud_burnPan = n ? Clamp(sx / n, -1, 1) : 0;
}

static void anim_yellow(float dt) {
    int lo = win_lo(1), hi = win_hi();
    if (st == ST_WON) { // arrived: no yellow route at all (it must not animate away)
        for (int j = lo; j <= hi; j++) for (int t = 0; t < NSEG; t++) S[j][t].fill = 0;
        return;
    }
    compute_targets();
    int ty = (int)kind, playing = st == ST_PLAY || st == ST_ZOOM;
    // burnt away behind the player: gone instantly
    if (st != ST_INTRO) {
        for (int j = 0; j < lv && j <= hi; j++) for (int t = 0; t < NSEG; t++) S[j][t].fill = 0;
        for (int q = ty, pj, p; (p = seg_parent(lv, q, &pj)) >= 0 && pj == lv; q = p) S[lv][p].fill = 0; // segments already passed
        if (playing) S[lv][ty].fill = 1;
    }
    int retracting = 0;
    for (int j = lo; j <= hi; j++) for (int t = 0; t < NSEG; t++) if (!S[j][t].target && S[j][t].fill > 0) retracting = 1;
    for (int j = hi; j >= lo; j--)
        for (int t = NSEG - 1; t >= 0; t--) {
            float *f = &S[j][t].fill;
            if (!S[j][t].target) {
                if (*f <= 0) continue;
                if (!child_lit(j, t, hi)) *f = fmaxf(0, *f - RETRACT_SPEED * dt / seg_len(j, t));
            } else if (*f < 1 && !retracting) {
                int pj, p = seg_parent(j, t, &pj); // a segment only fills once the one it grows out of is full
                int ok = p < 0 || S[pj][p].fill >= 0.999f;
                if (ok) *f = fminf(1, *f + FILL_SPEED * dt / seg_len(j, t));
            }
        }
}

static void draw_fills(void) {
    nuR = nuY = 0;
    Vector2 a, b;
    // red: every alternative. The tree ahead, plus branches not taken (which burn away once passed).
    if (st == ST_INTRO) {
        if (introSel)
            for (int t = 1; t <= 2; t++) { seg_geo(0, t, &a, &b); put_seg(uR, &nuR, MAXRS, a, b); }
    } else {
        int ty = (int)kind;
        for (int j = win_lo(3); j <= win_hi(); j++)
            for (int t = 0; t < NSEG; t++) {
                if (!seg_exists(j, t)) continue;
                if (S[j][t].burn >= 1) continue;
                int red = j > lv ? 1 : (j == lv && ty == 0 ? t >= 1 : S[j][t].alt);
                if (!red) continue;
                seg_geo(j, t, &a, &b);
                put_seg_j(uR, &nuR, MAXRS, j, t, Vector2Lerp(a, b, S[j][t].burn), b);
            }
    }
    // yellow: animated route
    int ylo = win_lo(1), yhi = win_hi(), ty = (int)kind;
    for (int j = ylo; j <= yhi; j++)
        for (int t = 0; t < NSEG; t++) {
            float f = S[j][t].fill;
            if (f <= 0) continue;
            seg_geo(j, t, &a, &b);
            if ((st == ST_PLAY || st == ST_ZOOM) && j == lv && t == ty) a = pos;
            put_seg_j(uY, &nuY, MAXYS, j, t, a, Vector2Lerp(a, b, f));
        }
}

// 0 -> 1 after the player has stopped at the end of the road: the road's glowing outlines fade away, leaving the circle
static float arrival_fade(void) { return ease(Clamp((winT - 0.8f) / 1.6f, 0, 1)); }

// Collects what is on screen and renders the whole path with the shader.
static void render_world(Camera2D c, int W, int H) {
    float m = 170.0f / c.zoom + PATH_W;
    viewR = (Rectangle){c.target.x - W / 2.0f / c.zoom - m, c.target.y - H / 2.0f / c.zoom - m,
                        W / c.zoom + 2 * m, H / c.zoom + 2 * m};
    draw_path();
    draw_fills();
    float camT[2] = {c.target.x, c.target.y}, camOff[2] = {c.offset.x, c.offset.y};
    float zoom = c.zoom, scrH = (float)H, pulse = 0.85f + 0.15f * sinf(tnow * 3), pw = PATH_W;
    SetShaderValue(pathShader, locCamT, camT, SHADER_UNIFORM_VEC2);
    static float lastOff[2], lastZoom, lastH, lastPw; // these rarely change: only upload when they do
    if (camOff[0] != lastOff[0] || camOff[1] != lastOff[1]) { lastOff[0] = camOff[0]; lastOff[1] = camOff[1]; SetShaderValue(pathShader, locCamOff, camOff, SHADER_UNIFORM_VEC2); }
    if (zoom != lastZoom) { lastZoom = zoom; SetShaderValue(pathShader, locZoom, &zoom, SHADER_UNIFORM_FLOAT); }
    if (scrH != lastH) { lastH = scrH; SetShaderValue(pathShader, locScrH, &scrH, SHADER_UNIFORM_FLOAT); }
    SetShaderValue(pathShader, locPulse, &pulse, SHADER_UNIFORM_FLOAT);
    float fade = st == ST_WON ? 1.0f - arrival_fade() : 1.0f; // the glowing outlines fade out after the arrival
    SetShaderValue(pathShader, locFade, &fade, SHADER_UNIFORM_FLOAT);
    float tm = fmodf(tnow, 1000.0f);
    SetShaderValue(pathShader, locTime, &tm, SHADER_UNIFORM_FLOAT);
    if (pw != lastPw) { lastPw = pw; SetShaderValue(pathShader, locPw, &pw, SHADER_UNIFORM_FLOAT); }
    SetShaderValue(pathShader, locNP, &nuP, SHADER_UNIFORM_INT);
    SetShaderValue(pathShader, locNR, &nuR, SHADER_UNIFORM_INT);
    SetShaderValue(pathShader, locNY, &nuY, SHADER_UNIFORM_INT);
    if (nuP) { SetShaderValueV(pathShader, locP, uP, SHADER_UNIFORM_VEC4, nuP); SetShaderValueV(pathShader, locPc, uPc, SHADER_UNIFORM_VEC3, nuP); }
    if (nuR) SetShaderValueV(pathShader, locR, uR, SHADER_UNIFORM_VEC4, nuR);
    if (nuY) SetShaderValueV(pathShader, locY, uY, SHADER_UNIFORM_VEC4, nuY);
    BeginShaderMode(pathShader);
    DrawRectangle(0, 0, W, H, WHITE);
    EndShaderMode();
}

// Text with a dark halo around it: readable over any glow, fill or path, without additive-glow artifacts.
static void ShadowText(const Face *fc, const char *t, Vector2 p, float fs, float sp, Color col, float a) {
    static float ox[2][12], oy[2][12];
    static int ready;
    if (!ready) {
        for (int ring = 0; ring < 2; ring++)
            for (int k = 0; k < 12; k++) {
                float ang = k * PI / 6 + ring * PI / 12; // stagger the rings for a smoother halo
                ox[ring][k] = cosf(ang); oy[ring][k] = sinf(ang);
            }
        ready = 1;
    }
    float r = fmaxf(2.0f, fs * 0.06f);
    Color shadow = Fade(BLACK, 0.45f * a);
    for (int ring = 0; ring < 2; ring++) {
        float rr = ring ? r * 0.5f : r;
        for (int k = 0; k < 12; k++) DrawTextEx(fc->f, t, (Vector2){p.x + ox[ring][k] * rr, p.y + oy[ring][k] * rr}, fs, sp, shadow);
    }
    // thin strokes: draw the text a few times with a sub-pixel offset to thicken them slightly
    float b = fc->bold * fs;
    Color c = Fade(col, a);
    if (b > 0) {
        DrawTextEx(fc->f, t, (Vector2){p.x - b, p.y}, fs, sp, c);
        DrawTextEx(fc->f, t, (Vector2){p.x + b, p.y}, fs, sp, c);
        DrawTextEx(fc->f, t, (Vector2){p.x, p.y - b}, fs, sp, c);
        DrawTextEx(fc->f, t, (Vector2){p.x, p.y + b}, fs, sp, c);
    } else DrawTextEx(fc->f, t, p, fs, sp, c);
}

static void NeonTextF(const Face *fc, const char *t, Vector2 c, float fs, Color col, float a) {
    fs *= uiS;
    float sp = fs * 0.06f;
    Vector2 sz = MeasureTextEx(fc->f, t, fs, sp);
    ShadowText(fc, t, (Vector2){c.x - sz.x / 2, c.y - sz.y / 2}, fs, sp, col, a);
}
static void NeonText(const char *t, Vector2 c, float fs, Color col, float a) { NeonTextF(&faceUI, t, c, fs, col, a); }

// one centred line made of pieces in different typefaces (e.g. "SCORE " in the UI face, the number in the number face)
typedef struct { const char *t; const Face *f; } Run;
static void NeonRun(Vector2 c, float fs, Color col, float a, const Run *r, int n) {
    fs *= uiS;
    float sp = fs * 0.06f, w[8], total = 0;
    for (int i = 0; i < n; i++) { w[i] = MeasureTextEx(r[i].f->f, r[i].t, fs, sp).x; total += w[i] + sp; }
    float x = c.x - total / 2;
    for (int i = 0; i < n; i++) {
        ShadowText(r[i].f, r[i].t, (Vector2){x, c.y - MeasureTextEx(r[i].f->f, r[i].t, fs, sp).y / 2}, fs, sp, col, a);
        x += w[i] + sp;
    }
}

static void draw_arrow(Vector2 c, int dir, int on) {
    float rot = dir < 0 ? 180 : (dir > 0 ? 0 : 270);
    Color col = PAL[0];
    if (on) {
        BeginBlendMode(BLEND_ADDITIVE);
        DrawPoly(c, 3, 46 * uiS, rot, Fade(col, 0.2f));
        EndBlendMode();
        DrawPoly(c, 3, 30 * uiS, rot, col);
    } else {
        DrawPolyLinesEx(c, 3, 30 * uiS, rot, 3, Fade(col, 0.45f));
    }
}

// ---------------------------------------------------------------- game logic
static void reset_game(void) {
    memset(S, 0, sizeof S);
    gen = 0;
    ensure_gen(10);
    pos = L[0].P;
    to = L[0].F;
    kind = K_APPROACH;
    lv = 0; choice = 0; lastSide = 1; speed = baseSpeed(0);
    introSel = 0; runT = 0; winT = 0; firstMerge = -1; mergesPassed = 0; mergeZoom = 1;
    deathBurn = 0; flash = 0; shake = 0;
    for (int i = 0; i < MAXP; i++) parts[i].life = 0;
    st = ST_INTRO;
}

static void spawn_spark(Vector2 p, Vector2 v, float life, Color c, float size) {
    for (int i = 0; i < MAXP; i++)
        if (parts[i].life <= 0) { parts[i] = (Particle){p, v, life, life, c, size}; return; }
}

// the end of the road: the player stops at the dead end and wins
static void win(void) {
    st = ST_WON; winT = 0;
    if (lv > best[hard]) best[hard] = lv;
    sfxp(sWin, 0, 1.0f);
}

static void crash(void) {
    st = ST_DEAD; deadT = 0; flash = 0.5f; shake = 1;
    sfxp(sCrash, 0, 1.0f);
    if (lv > best[hard]) best[hard] = lv; // the score is the number of forks passed
    for (int i = 0; i < MAXP; i++) {
        float a = frand(0, 2 * PI), v = frand(80, 520);
        float life = frand(0.5f, 1.4f);
        parts[i] = (Particle){pos, {cosf(a) * v, sinf(a) * v}, life, life,
                              GetRandomValue(0, 2) ? PAL[(lv + 1) % PAL_N] : WHITE, 1};
    }
}

// the player reached the end of a fork (the node after its last segment): score and move on to the next fork
static int pass_fork(void) {
    if (L[lv].merge) mergesPassed++;
    lv++; scorePop = 1;
    for (int k = 0; k < 28; k++) { // celebratory burst at the node we just crossed
        float a = frand(0, 2 * PI), v = frand(60, 320);
        spawn_spark(pos, (Vector2){cosf(a) * v, sinf(a) * v}, frand(0.3f, 0.7f), GetRandomValue(0, 1) ? FYEL : PAL[lv % PAL_N], 0.6f);
    }
    ensure_gen(lv + 8);
    if (lv >= gen - 1 && lv != WIN_SCORE) { crash(); return 0; } // end of generated world
    to = L[lv].F; kind = K_APPROACH;
    speed = baseSpeed(lv);
    return 1;
}

// merge fork: at the fork pick the left (segments 1-3) or right (4-6) branch, then walk its segments in order
static int merge_step(void) {
    Vector2 a;
    if (kind == K_APPROACH) {
        S[lv][0].burning = 1; // the approach behind us can no longer be selected
        int left = eff_sel(lv) < 0;
        for (int t = left ? 4 : 1; t <= (left ? 6 : 3); t++) S[lv][t].burning = S[lv][t].alt = 1; // the other branch burns away
        kind = left ? (Kind)1 : (Kind)4;
        seg_geo(lv, kind, &a, &to);
        return 1;
    }
    S[lv][kind].burning = 1;
    if (kind == 3 || kind == 6) return pass_fork();
    kind = (Kind)(kind + 1);
    seg_geo(lv, kind, &a, &to);
    return 1;
}

static void advance(float dt) {
    if (L[lv].end && kind == K_APPROACH) // the last stretch: ease to a gentle stop at the end of the road
        speed = fmaxf(35.0f, baseSpeed(lv) * Clamp(Vector2Distance(pos, to) / 380.0f, 0, 1));
    float rem = speed * dt;
    while (rem > 0 && (st == ST_PLAY || st == ST_ZOOM)) {
        float d = Vector2Distance(pos, to);
        if (d > rem) {
            Vector2 before = pos;
            pos = Vector2Add(pos, Vector2Scale(Vector2Normalize(Vector2Subtract(to, pos)), rem));
            int gt = (int)kind;
            // stepping onto a gap the orb is far from: fall. (Once on it, the orb waits there until you are through.)
            if (has_gap(lv, gt) && gap_deadly(lv, gt) && !in_gap_area(lv, gt, before) && in_gap_area(lv, gt, pos)) { crash(); return; }
            break;
        }
        pos = to; rem -= d;
        if (L[lv].end) { win(); return; }
        if (L[lv].merge) { if (!merge_step()) return; continue; }
        switch (kind) {
        case K_APPROACH: { // reached the fork: commit to the chosen side
            Level *m = &L[lv];
            int c = eff_sel(lv);
            S[lv][0].burning = 1; // the approach behind us can no longer be selected
            if (c == m->side) {
                to = m->N; kind = K_TRUE;
                // the dead ends can't be selected anymore: red, and burning away from their roots
                S[lv][2].burning = S[lv][3].burning = S[lv][2].alt = S[lv][3].alt = 1;
                if (m->three) S[lv][4].burning = S[lv][5].burning = S[lv][4].alt = S[lv][5].alt = 1;
            } else {
                // wrong turn: nothing burns, the real path is revealed in red
                sfxp(sWrong, c, 1.0f);
                S[lv][1].alt = 1;
                if (m->three && c != m->dirD) { to = m->E1; kind = K_E1; S[lv][2].alt = S[lv][3].alt = 1; }
                else { to = m->D1; kind = K_D1; if (m->three) S[lv][4].alt = S[lv][5].alt = 1; }
            }
        } break;
        case K_TRUE:
            S[lv][1].burning = 1; // the branch we just left
            if (!pass_fork()) return;
            break;
        case K_D1: to = L[lv].D2; kind = K_D2; break;
        case K_D2: crash(); return;
        case K_E1: to = L[lv].E2; kind = K_E2; break;
        case K_E2: crash(); return;
        default: break; // K_X6 only occurs in merge forks (handled in merge_step)
        }
    }
}

// ---------------------------------------------------------------- input helpers
static float dist_seg(Vector2 p, Vector2 a, Vector2 b) {
    Vector2 ab = Vector2Subtract(b, a), ap = Vector2Subtract(p, a);
    float h = Clamp(Vector2DotProduct(ap, ab) / fmaxf(Vector2DotProduct(ab, ab), 1e-6f), 0, 1);
    return Vector2Distance(p, Vector2Add(a, Vector2Scale(ab, h)));
}
// mouse position -> side, by screen thirds (3-way fork) or halves (2-way fork)
static int screen_dir(float x, int W, int three) {
    if (three) return x < W / 3.0f ? -1 : (x > 2 * W / 3.0f ? 1 : 0);
    return x < W / 2.0f ? -1 : 1;
}
// tap on the path: which branch of fork j is closest to the tapped world position?
static int branch_dir_at(Vector2 w, int j) {
    Level *m = &L[j];
    if (w.y > m->F.y - 40) { // below the fork: decide by the horizontal offset from the fork
        float dx = w.x - m->F.x;
        return m->three ? (dx < -70 ? -1 : (dx > 70 ? 1 : 0)) : (dx < 0 ? -1 : 1);
    }
    if (m->merge) { // left branch (types 1-3) or right branch (4-6): whichever is nearer
        float dl = 1e9f, dr = 1e9f;
        for (int t = 1; t <= 6; t++) {
            Vector2 a, b;
            seg_geo(j, t, &a, &b);
            float d = dist_seg(w, a, b);
            if (t <= 3) dl = fminf(dl, d); else dr = fminf(dr, d);
        }
        return dl < dr ? -1 : 1;
    }
    float best = dist_seg(w, m->F, m->N);
    int dir = m->side;
    float dD = fminf(dist_seg(w, m->F, m->D1), dist_seg(w, m->D1, m->D2));
    if (dD < best) { best = dD; dir = m->dirD; }
    if (m->three) {
        float dE = fminf(dist_seg(w, m->F, m->E1), dist_seg(w, m->E1, m->E2));
        if (dE < best) dir = -m->side - m->dirD; // the remaining direction
    }
    return dir;
}

static float z0 = 1;
static Vector2 t0;
// touch tracking (swipe / tap classification)
static int wasTouch;
static Vector2 tStart, tLast;
static float tStartT, lastTouchT = -10;

// ---------------------------------------------------------------- platform glue (the only place with #ifdefs)
// is the game window in the background (another workspace/window, minimized, hidden tab)?
static int platform_in_background(void) {
#ifdef PLATFORM_WEB
    return EM_ASM_INT({ return document.hidden ? 1 : 0; });
#else
    return !IsWindowFocused() || IsWindowHidden() || IsWindowMinimized();
#endif
}

static void platform_pre_frame(void) {
#ifdef PLATFORM_WEB
    // follow the browser window size (responsive canvas), resizing only when the window changed
    int iw = EM_ASM_INT({ return window.innerWidth; }), ih = EM_ASM_INT({ return window.innerHeight; });
    static int lastIw, lastIh;
    if (iw > 0 && ih > 0 && (iw != lastIw || ih != lastIh)) {
        lastIw = iw; lastIh = ih;
        if (iw != GetScreenWidth() || ih != GetScreenHeight()) SetWindowSize(iw, ih);
    }
#endif
}

static void platform_init(void) {
#ifndef PLATFORM_WEB
    SetTargetFPS(60);
#endif
#ifndef WT_NO_KEYBOARD
    SetExitKey(KEY_ESCAPE);
#endif
#if defined(WT_NO_KEYBOARD)
    kbHint = 0;
#elif defined(PLATFORM_WEB)
    // phones/tablets (coarse primary pointer) start without keyboard hints; a key press brings them back
    kbHint = !EM_ASM_INT({ return (window.matchMedia && window.matchMedia('(pointer: coarse)').matches) ? 1 : 0; });
#endif
}

static void platform_run(void (*frame)(void)) {
#ifdef PLATFORM_WEB
    emscripten_set_main_loop(frame, 0, 1); // the browser drives the frames
#else
    while (!WindowShouldClose()) frame();
#endif
}

// ---------------------------------------------------------------- input
typedef struct { int pick, confirm, mute, fullscreen; } KeyInput;

// Keyboard: compiled out where there is none. Any key press shows the keyboard hints.
static KeyInput poll_keyboard(void) {
    KeyInput k = {0};
#ifndef WT_NO_KEYBOARD
    while (GetKeyPressed()) kbHint = 1;
    k.confirm = IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER);
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A)) k.pick = -1;
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)) k.pick = 1;
    k.mute = IsKeyPressed(KEY_M);
    k.fullscreen = IsKeyPressed(KEY_F11);
#endif
    return k;
}

typedef struct {
    int keyPick;             // -1/+1 from a key or a swipe: steps the selection
    int pick;                // keyPick, or the side that was clicked/tapped (in the intro this picks the difficulty)
    int confirm, click, moved, mute, fullscreen;
    Vector2 cpos, mpos;      // where the click/tap happened; the mouse position
} Input;

static Input poll_input(int W, int H) {
    Input in = {0};
    // Touch: classify a finger lift as a swipe (left/right) or a tap. Uses the touch API directly, so it does not
    // depend on the platform emulating a mouse.
    int touching = GetTouchPointCount() > 0, swipe = 0, tapEvent = 0;
    if (touching) {
        Vector2 tp = GetTouchPosition(0);
        if (!wasTouch) { tStart = tp; tStartT = tnow; }
        tLast = tp;
        lastTouchT = tnow;
    } else if (wasTouch) {
        Vector2 d = Vector2Subtract(tLast, tStart);
        float dur = tnow - tStartT, thr = fmaxf(36.0f, 0.05f * fminf((float)W, (float)H));
        if (fabsf(d.x) > thr && fabsf(d.x) > 1.3f * fabsf(d.y) && dur < 0.9f) swipe = d.x < 0 ? -1 : 1;
        else if (Vector2Length(d) < 28.0f && dur < 0.6f) tapEvent = 1;
    }
    wasTouch = touching;
    int touchRecent = touching || tnow - lastTouchT < 0.5f; // ignore the mouse emulated from touches

    KeyInput k = poll_keyboard();
    in.keyPick = swipe ? swipe : k.pick; // a swipe steps the selection exactly like a key press
    in.mute = k.mute;
    in.fullscreen = k.fullscreen;

    // Mouse: click a path / side, or just move left or right to select.
    in.click = (!touchRecent && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) || tapEvent;
    in.mpos = GetMousePosition();
    in.cpos = tapEvent ? tLast : in.mpos;
    Vector2 md = GetMouseDelta();
    in.moved = !touchRecent && fabsf(md.x) + fabsf(md.y) > 2;
    in.confirm = k.confirm || in.click;
    in.pick = in.click ? (in.cpos.x < W / 2 ? -1 : 1) : in.keyPick;
    return in;
}

// Selection: in play it steps/jumps/taps the branch, in the intro it previews the difficulty.
static void apply_selection(const Input *in, int W) {
    if (st == ST_PLAY) {
        // the next fork decides the layout: 3-way forks step left/middle/right, 2-way forks jump to a side
        int upc = next_fork();
        if (upc < gen && L[upc].end) return; // no more forks
        int three = upc < gen && L[upc].three, nc = choice, changed = 0;
        if (in->keyPick) { nc = three ? (int)Clamp((float)(choice + in->keyPick), -1, 1) : in->keyPick; changed = 1; }
        else if (upc < gen) {
            if (in->click) { nc = branch_dir_at(GetScreenToWorld2D(in->cpos, cam), upc); changed = 1; }
            else if (in->moved) { nc = screen_dir(in->mpos.x, W, three); changed = 1; }
        }
        if (changed) {
            if (nc != choice) sfxp(sPick, nc, 1.0f + 0.08f * nc);
            choice = nc;
            if (nc) lastSide = nc;
        }
    }
    if (st == ST_INTRO) {
        if (in->pick) introSel = in->pick;
        if (in->moved) introSel = in->mpos.x < W / 2 ? -1 : 1;
    }
}

// ---------------------------------------------------------------- update
static float zoomE; // 0..1 ease of the intro -> gameplay zoom transition

static void update_particles(float dt) {
    for (int i = 0; i < MAXP; i++)
        if (parts[i].life > 0) {
            parts[i].life -= dt;
            parts[i].pos = Vector2Add(parts[i].pos, Vector2Scale(parts[i].vel, dt));
            parts[i].vel = Vector2Scale(parts[i].vel, 1 - 1.5f * dt);
        }
    if (st == ST_PLAY || st == ST_ZOOM) { // sparks thrown off by the burning fuse head
        for (int k = 0; k < 2; k++) {
            float a = frand(0, 2 * PI), v = frand(30, 170);
            Color c = GetRandomValue(0, 2) ? (GetRandomValue(0, 1) ? FYEL : (Color){255, 150, 30, 255}) : WHITE;
            spawn_spark(pos, (Vector2){cosf(a) * v, sinf(a) * v - 30}, frand(0.25f, 0.55f), c, 0.45f);
        }
    }
}

// drive the streamed audio bed from the game state
static void update_audio(int W) {
    aud_drone = (st == ST_PLAY || st == ST_ZOOM) ? 1.0f : (st == ST_INTRO || st == ST_WON ? 0.35f : 0.0f);
    aud_speed = speed;
    aud_lean = 0;
    if (st == ST_PLAY || st == ST_ZOOM) {
        int upc = next_fork();
        if (upc < gen) {
            aud_pan = (float)eff_sel(upc);
            aud_lean = kind == K_APPROACH ? Clamp(1 - Vector2Distance(pos, L[lv].F) / 450.0f, 0, 1) : 0.0f;
        }
    }
    burn_audio(W);
    bed_update();
}

static void update_camera(float dt, int W, int H, float zs) {
    float introZ = fminf(0.70f * H / 760.0f, 0.9f * W / 640.0f);
    // start zoomed out (easier), then slowly zoom in until the maximum is reached
    const Diff *dif = &DIFF[hard];
    if (firstMerge < 0)
        for (int j = lv; j <= win_hi(); j++) if (L[j].merge) { firstMerge = j; break; }
    float mzTarget = 1;
    if (firstMerge >= 0 && lv >= firstMerge - 3) { // zoom out shortly before the first merge fork, then back in over time
        float back = fmaxf(Clamp((mergesPassed - 2) / 4.0f, 0, 1), Clamp((lv - firstMerge - 20) / 15.0f, 0, 1));
        mzTarget = Lerp(0.72f, 1.0f, back);
    }
    mergeZoom += (mzTarget - mergeZoom) * fminf(1, 1.2f * dt);
    float Zg = Lerp(dif->zStart, dif->zMax, ease(Clamp(runT / dif->zSecs, 0, 1))) * zs * mergeZoom;
    cam.offset = (Vector2){W / 2.0f, H / 2.0f};
    if (st == ST_INTRO) {
        float k = fminf(1, 5 * dt);
        cam.zoom += (introZ - cam.zoom) * k;
        cam.target = Vector2Lerp(cam.target, (Vector2){0, -290}, k);
        z0 = cam.zoom; t0 = cam.target;
    } else if (st == ST_ZOOM) {
        cam.zoom = Lerp(z0, Zg, zoomE);
        Vector2 ft = {pos.x, pos.y - 0.2f * H / cam.zoom};
        cam.target = Vector2Lerp(t0, ft, zoomE);
    } else if (st == ST_PLAY) {
        cam.zoom += (Zg - cam.zoom) * fminf(1, 2 * dt);
        cam.target.x += (pos.x - cam.target.x) * fminf(1, 4 * dt);
        cam.target.y = pos.y - 0.2f * H / cam.zoom;
    }
}

static void update_game(float dt, int W, int H, float zs, const Input *in) {
    if (st == ST_ZOOM || st == ST_PLAY) runT += dt;
    zoomE = 0;
    if (st == ST_INTRO) {
        if (in->pick) {
            startSide = choice = lastSide = in->pick; hard = in->pick > 0;
            gen = 0; ensure_gen(10); firstMerge = -1;
            speed = baseSpeed(0);
            st = ST_ZOOM; zoomT = 0; sfxp(sStart, 0, DIFF[hard].startPitch);
        }
    } else if (st == ST_ZOOM) {
        zoomT += dt;
        zoomE = ease(Clamp(zoomT / 1.6f, 0, 1));
        float sm = Clamp(zoomT / 1.2f, 0, 1);
        speed = baseSpeed(0) * sm * sm;
        advance(dt);
        if (zoomT >= 1.6f) { st = ST_PLAY; speed = baseSpeed(lv); }
    } else if (st == ST_PLAY) {
        advance(dt);
    } else if (st == ST_WON) {
        winT += dt;
        if (winT > 1.5f && in->confirm) reset_game();
    } else if (st == ST_DEAD) {
        deadT += dt;
        if (deadT > 0.6f && !deathBurn) { // after a beat, every remaining path burns away from its root
            deathBurn = 1;
            for (int j = win_lo(5); j <= win_hi(); j++)
                for (int t = 0; t < NSEG; t++) S[j][t].burning = 1;
        }
        if (deadT > 0.7f && in->confirm) reset_game();
    }
    update_orbs(dt);
    update_particles(dt);
    scorePop = fmaxf(0, scorePop - dt * 3);
    update_audio(W);
    flash = fmaxf(0, flash - dt);
    shake = fmaxf(0, shake - dt * 2.5f);
    anim_yellow(dt);
    update_burn(dt);
    update_camera(dt, W, H, zs);
}

// ---------------------------------------------------------------- draw
static void draw_arrows(int W, int H, int sel, int three) {
    float y = H - 60.0f * uiS;
    draw_arrow((Vector2){W / 2.0f - 110 * uiS, y}, -1, sel == -1);
    draw_arrow((Vector2){W / 2.0f + 110 * uiS, y}, 1, sel == 1);
    if (three) draw_arrow((Vector2){W / 2.0f, y}, 0, sel == 0);
}

static void NeonTop(const Face *fc, const char *t, int W, float y, float fs, Color col, float a) { NeonTextF(fc, t, (Vector2){W / 2.0f, y * uiS}, fs, col, a); }

// EASY / HARD above the tips of the intro fork (world space; the fork's branches are straight, so a branch's tip is N or D2)
static void draw_intro_labels(float a) {
    Vector2 tip[2] = {L[0].side > 0 ? L[0].D2 : L[0].N, L[0].side > 0 ? L[0].N : L[0].D2};
    for (int k = 0; k < 2; k++) {
        float fs = 44, sp = fs * 0.1f;
        Vector2 sz = MeasureTextEx(faceUI.f, DIFF[k].name, fs, sp);
        ShadowText(&faceUI, DIFF[k].name, (Vector2){tip[k].x - sz.x / 2, tip[k].y - PATH_W / 2 - 20 - sz.y}, fs, sp, PAL[DIFF[k].palIdx], a);
    }
}

// A blob like a missing piece of path, drawn like a cross-section of the tube as wide as the path: neon outline in
// the path's colour, a dark rim, and the fill inside (a little brighter at its edge, like the path fills).
// Used for the orbs in merge forks and for the circle that marks the arrival at the end of the road.
static void draw_blob(Vector2 c, float scale, float vis, float edgeVis, Color edge, Color fill) {
    float r = PATH_W / 2 * scale, rf = fmaxf(r - 8 * scale, 0);
    BeginBlendMode(BLEND_ADDITIVE);
    DrawCircleGradient(c, PATH_W * 0.95f * scale, Fade(edge, 0.6f * vis * edgeVis), BLANK); // glow, like the path's edge glow
    EndBlendMode();
    DrawCircleSector(c, r, 0, 360, 64, Fade(edge, vis * edgeVis));
    DrawCircleSector(c, fmaxf(r - 5 * scale, 0), 0, 360, 64, Fade(BLACK, vis * edgeVis));
    DrawCircleSector(c, rf, 0, 360, 64, Fade(mix_color(fill, WHITE, 0.25f), vis));
    DrawCircleSector(c, fmaxf(rf - 7 * scale, 0), 0, 360, 64, Fade(fill, vis));
}

static void draw_world(Camera2D c, int W, int H) {
    render_world(c, W, H);
    BeginMode2D(c);
    if (st == ST_INTRO || st == ST_ZOOM) {
        float a = st == ST_INTRO ? 0.8f + 0.2f * sinf(tnow * 4) : Clamp(1 - zoomE * 2.2f, 0, 1);
        if (a > 0) draw_intro_labels(a);
    }
    for (int j = win_lo(2); j <= win_hi(); j++) { // the orb(s) travelling between the gaps of merge forks
        if (!L[j].merge) continue;
        Vector2 ca = Vector2Lerp(L[j].D1, L[j].D2, 0.5f), cb = Vector2Lerp(L[j].E1, L[j].E2, 0.5f);
        if (!CheckCollisionPointRec(Vector2Lerp(ca, cb, 0.5f), viewR)) continue;
        // hidden while it sits in a gap (the filled gap is the orb); it appears as it leaves and disappears as it arrives
        float vis = Clamp((fminf(L[j].orbF, 1.0f - L[j].orbF) - 0.05f) / 0.15f, 0, 1);
        // when the branches burn away the orb shrinks fast: the burn front passing the gaps (20%..50% of the middle segments)
        float burnt = fmaxf(S[j][2].burn, S[j][5].burn);
        float scale = Clamp(1.0f - (burnt - 0.2f) / 0.3f, 0, 1);
        if (vis <= 0 || scale <= 0.01f) continue;
        Color oc = mix_color(FREDC, FYEL, L[j].orbHue); // red <-> yellow, following the branch it is heading to
        Vector2 op = Vector2Lerp(ca, cb, L[j].orbF);
        draw_blob(op, scale, vis, 1.0f, PAL[(j + 1) % PAL_N], oc);
    }
    BeginBlendMode(BLEND_ADDITIVE);
    if (st == ST_PLAY || st == ST_ZOOM) { // glowing head of the fuse
        DrawCircleGradient(pos, 70, Fade(FYEL, 0.55f), BLANK);
        DrawCircleGradient(pos, 26, Fade(WHITE, 0.8f), BLANK);
    }
    for (int i = 0; i < MAXP; i++)
        if (parts[i].life > 0) {
            float f = parts[i].life / parts[i].max;
            DrawCircleGradient(parts[i].pos, (4 + 9 * f) * parts[i].size, Fade(parts[i].col, f), BLANK);
        }
    EndBlendMode();
    EndMode2D();
}

// fog of war: the view shrinks as you progress
static void draw_fog(int W, int H) {
    float k = st == ST_ZOOM ? zoomE : (st == ST_INTRO ? 0.0f : (st == ST_WON ? Clamp(1.0f - winT, 0, 1) : 1.0f));
    if (k <= 0) return;
    const Diff *dif = &DIFF[hard];
    Vector2 sp = GetWorldToScreen2D(pos, cam);
    float R = Clamp(dif->fogMax - dif->fogStep * lv, dif->fogMin, dif->fogMax) * H / 900.0f;
    float ctr[2] = {sp.x, H - sp.y}; // gl_FragCoord has a bottom-left origin
    SetShaderValue(fogShader, locCenter, ctr, SHADER_UNIFORM_VEC2);
    SetShaderValue(fogShader, locRadius, &R, SHADER_UNIFORM_FLOAT);
    SetShaderValue(fogShader, locStrength, &k, SHADER_UNIFORM_FLOAT);
    BeginShaderMode(fogShader);
    DrawRectangle(0, 0, W, H, WHITE);
    EndShaderMode();
}

static void draw_hud(int W, int H) {
    const Diff *dif = &DIFF[hard];
    if (st == ST_INTRO) {
        NeonTop(&faceTitle, "WRONG TURN", W, 44, 52, PAL[1], 1);
        NeonText("CHOOSE YOUR PATH", (Vector2){W / 2.0f, H - 118.0f * uiS}, 26, WHITE, 0.95f);
        draw_arrows(W, H, introSel, 0);
    } else {
        NeonTop(&faceNum, TextFormat("%d", lv), W, 56, 72 * (1 + 0.4f * scorePop * scorePop), PAL[(lv + 1) % PAL_N], 1);
        NeonTop(&faceUI, dif->name, W, 112, 24, PAL[dif->palIdx], 1.0f);
        if (best[hard] > 0) {
            const Run r[] = {{"BEST ", &faceUI}, {TextFormat("%d", best[hard]), &faceNum}};
            NeonRun((Vector2){W / 2.0f, 142 * uiS}, 22, WHITE, 0.9f, r, 2);
        }
        int upc = next_fork();
        if (st != ST_DEAD && st != ST_WON && !(upc < gen && L[upc].end))
            draw_arrows(W, H, upc < gen ? eff_sel(upc) : choice, upc < gen && L[upc].three);
    }
    if (st == ST_DEAD && deadT > 0.35f) {
        float a = Clamp((deadT - 0.35f) * 3, 0, 1);
        DrawRectangle(0, 0, W, H, Fade(BLACK, 0.55f * a));
        NeonTextF(&faceTitle, "WRONG TURN", (Vector2){W / 2.0f, H * 0.4f}, 84, PAL[1], a);
        const Run r[] = {{"SCORE ", &faceUI}, {TextFormat("%d", lv), &faceNum}, {"   BEST ", &faceUI}, {TextFormat("%d", best[hard]), &faceNum}};
        NeonRun((Vector2){W / 2.0f, H * 0.5f}, 36, WHITE, a, r, 4);
        if (deadT > 0.7f && fmodf(tnow, 1.2f) < 0.8f)
            NeonText(kbHint ? "SPACE OR CLICK TO CHOOSE AGAIN" : "TAP TO CHOOSE AGAIN", (Vector2){W / 2.0f, H * 0.6f}, 30, PAL[0], 0.9f);
    }
    if (st == ST_WON && winT > 1.0f) { // the ending
        float a = Clamp((winT - 1.0f) * 2, 0, 1), y = H * 0.44f, gap = 46 * uiS;
        DrawRectangle(0, 0, W, H, Fade(BLACK, 0.55f * a));
        NeonTextF(&faceTitle, "RIGHT TURN", (Vector2){W / 2.0f, H * 0.28f}, 84, PAL[2], a);
        NeonText(hard ? "That's impressive!" : "You're amazing!", (Vector2){W / 2.0f, y}, 36, WHITE, a);
        NeonText("But every trip has an end of the road.", (Vector2){W / 2.0f, y + gap}, 28, WHITE, a * 0.95f);
        NeonText("Take a rest before your next turn.", (Vector2){W / 2.0f, y + 2 * gap}, 28, WHITE, a * 0.95f);
        if (!hard) NeonText("You can try the hard mode next time.", (Vector2){W / 2.0f, y + 3.4f * gap}, 26, PAL[1], a);
        if (winT > 2.5f && fmodf(tnow, 1.2f) < 0.8f)
            NeonText(kbHint ? "SPACE OR CLICK TO PLAY AGAIN" : "TAP TO PLAY AGAIN", (Vector2){W / 2.0f, H * 0.86f}, 26, PAL[0], 0.9f);
    }
    if (flash > 0) DrawRectangle(0, 0, W, H, Fade(WHITE, flash));
}

static void frame(void) {
    platform_pre_frame();
    int bg = platform_in_background();
    if (bg != inBackground) { // silence everything while in the background, restore when back
        inBackground = bg;
        if (audioOK) SetMasterVolume(bg ? 0.0f : 1.0f);
    }
    audio_warm();
    float dt = fminf(GetFrameTime(), 0.05f);
    int W = GetScreenWidth(), H = GetScreenHeight();
    tnow += dt;
    float vs = fminf(W / 800.0f, H / 900.0f);
    uiS = Clamp(vs, 0.55f, 1.3f);
    float zs = Clamp(powf(vs, 0.75f), 0.55f, 1.5f); // gameplay zoom scales with the screen size

    Input in = poll_input(W, H);
    if (in.fullscreen) ToggleFullscreen();
    if (in.mute) muted = !muted;
    apply_selection(&in, W);
    update_game(dt, W, H, zs, &in);

    Camera2D c = cam;
    if (shake > 0) {
        c.target.x += frand(-1, 1) * shake * 18;
        c.target.y += frand(-1, 1) * shake * 18;
    }
    BeginDrawing();
    ClearBackground(BLACK);
    draw_world(c, W, H);
    draw_fog(W, H);
    draw_hud(W, H);
    if (st == ST_WON) { // arrived safely: the player stops and leaves a circle. Drawn above the ending's dark overlay so it stays vivid
        BeginMode2D(c);
        draw_blob(pos, 1.0f, 1.0f, 1.0f, PAL[lv % PAL_N], FYEL);
        EndMode2D();
    }
    EndDrawing();
}

// ---------------------------------------------------------------- main
// loads the first font file that exists (glyphs are rasterized at 128 px and scaled down with mipmaps)
static Font load_font(const char *const *paths, int n, Font fallback) {
    for (int i = 0; i < n; i++) {
        if (!FileExists(paths[i])) continue;
        Font f = LoadFontEx(paths[i], 128, NULL, 0);
        if (f.texture.id == GetFontDefault().texture.id) continue; // raylib returns the default font when loading fails
        GenTextureMipmaps(&f.texture);
        SetTextureFilter(f.texture, TEXTURE_FILTER_TRILINEAR);
        return f;
    }
    return fallback;
}

int main(void) {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(800, 900, "WRONG TURN");
    pathShader = LoadShaderFromMemory(NULL, pathFs);
    locCamT = GetShaderLocation(pathShader, "camT"); locCamOff = GetShaderLocation(pathShader, "camOff");
    locZoom = GetShaderLocation(pathShader, "zoom"); locScrH = GetShaderLocation(pathShader, "scrH");
    locPulse = GetShaderLocation(pathShader, "pulse"); locTime = GetShaderLocation(pathShader, "time"); locPw = GetShaderLocation(pathShader, "pw"); locFade = GetShaderLocation(pathShader, "pathFade");
    locNP = GetShaderLocation(pathShader, "nP"); locNR = GetShaderLocation(pathShader, "nR"); locNY = GetShaderLocation(pathShader, "nY");
    locP = GetShaderLocation(pathShader, "pSeg"); locPc = GetShaderLocation(pathShader, "pCol");
    locR = GetShaderLocation(pathShader, "rSeg"); locY = GetShaderLocation(pathShader, "ySeg");
    fogShader = LoadShaderFromMemory(NULL, fogFs);
    locCenter = GetShaderLocation(fogShader, "center");
    locRadius = GetShaderLocation(fogShader, "radius");
    locStrength = GetShaderLocation(fogShader, "strength");
    audio_init();
    platform_init();

    // typefaces (assets/fonts); the UI face falls back to a system font, the others to the UI face
    static const char *const uiFonts[] = {"assets/fonts/Tilt_Neon/TiltNeon-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", "/usr/share/fonts/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Bold.ttf"};
    static const char *const titleFonts[] = {"assets/fonts/neoneon/Neoneon.otf"};
    static const char *const numFonts[] = {"assets/fonts/beon/Beon-Regular.ttf"};
    faceUI = (Face){load_font(uiFonts, 4, GetFontDefault()), 0.0f};
    faceTitle = (Face){load_font(titleFonts, 1, faceUI.f), 0.012f};
    faceNum = (Face){load_font(numFonts, 1, faceUI.f), 0.012f};

    SetRandomSeed(((unsigned)time(NULL) ^ (unsigned)(GetTime() * 1000000.0)) * 7919u + 13u);
    reset_game();
    cam = (Camera2D){0};
    cam.zoom = 1;
    cam.target = (Vector2){0, -290};
    t0 = cam.target;

    platform_run(frame);
    if (audioOK) { UnloadAudioStream(bed); CloseAudioDevice(); }
    UnloadShader(fogShader);
    UnloadShader(pathShader);
    CloseWindow();
    return 0;
}
