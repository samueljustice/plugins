#include "MonsterView.h"

#include "Params.h"
#include "RoarschachLookAndFeel.h"

#include <cmath>
#include <vector>

namespace
{
    constexpr float c_quietLevel = 0.001f;      // -60 dBFS: below this the input counts as silence
    constexpr float c_sleepAfterSeconds = 1.5f;
    // Hosts stop calling processBlock when the transport stops: the features then freeze at their
    // last values. No new chunk for this long counts as silence (longer than any host buffer).
    constexpr float c_staleAfterSeconds = 0.5f;
    constexpr int c_frameHz = 30;                // GL frames per second while awake
    constexpr int c_idleFrameDivider = 2;        // 15 Hz once asleep and settled (the clouds still drift; sound coming back is noticed)
    constexpr int c_failAfterTicks = 3 * c_frameHz; // 3 s on screen without a GL context
    constexpr float c_maxFrameSeconds = 0.1f;   // a stalled frame doesn't make the animation jump
    constexpr double c_timeWrapSeconds = 2048.0; // keeps the shader's time small enough for float precision
    constexpr float c_detailBoostRangeDb = 24.0f;
    constexpr float c_detailCutRangeDb = 18.0f;

    // One-pole smoothing coefficient for a time constant, given the frame time.
    float smoothing (float dt, float seconds)
    {
        return 1.0f - std::exp (-dt / std::max (seconds, 1.0e-4f));
    }

    // Features come from another thread: never let a NaN or a wild value reach the shader.
    float safe (float v, float lo, float hi)
    {
        return std::isfinite (v) ? juce::jlimit (lo, hi, v) : 0.0f;
    }

    void follow (float& state, float target, float dt, float attackSeconds, float releaseSeconds)
    {
        state += (target - state) * smoothing (dt, target > state ? attackSeconds : releaseSeconds);

        if (! std::isfinite (state))
            state = 0.0f;
    }

    // GLSL written for GLSL 1.10 (legacy contexts) so that OpenGLHelpers::translate*ToV3 turns it into
    // valid 1.50 core code: float literals everywhere, no round/tanh, no const arrays, and none of the
    // keywords the translation rewrites inside names or comments.
    const char* const c_vertexShader = R"GLSL(
attribute vec2 aPosition;
varying vec2 vUv;

void main()
{
    vUv = aPosition * 0.5 + 0.5;
    gl_Position = vec4 (aPosition, 0.0, 1.0);
}
)GLSL";

    const char* const c_fragmentShader = R"GLSL(
varying vec2 vUv;

uniform vec2 uRes;
uniform float uPxScale;
uniform float uRadius;
uniform float uTime;
uniform float uJaw;
uniform float uGrowl;
uniform float uGrowlMod;
uniform float uLiquid;
uniform float uLiquidPhase;
uniform float uRoar;
uniform float uRoarDrive;
uniform float uSub;
uniform float uSubLevel;
uniform float uSubPhase;
uniform float uPitch;
uniform float uSize;
uniform float uGravelOn;
uniform float uDetailLow;
uniform float uDetailHigh;
uniform float uBoost;
uniform float uCut;
uniform float uSleep;
uniform float uBlink;
uniform float uLevel;
uniform float uBands[32];
uniform float uSwirlNotch;
uniform float uSwirlAmt;
uniform float uEnsWidth;
uniform float uDeess;
uniform float uTame;
uniform float uGateClosed;
uniform float uOvLevel;
uniform float uOvHarm[5];
uniform float uOvReplace;
uniform float uRing;
uniform float uRingFlicker;
uniform float uRingBody;
uniform float uStrain;
uniform float uStrainPulse;
uniform float uClip;
uniform vec3 uColScope;
uniform vec3 uColGrid;
uniform vec3 uColOuter;
uniform vec3 uColPitch;
uniform vec3 uColGrowl;
uniform vec3 uColLiquid;
uniform vec3 uColRoar;
uniform vec3 uColSub;
uniform vec3 uColDetail;
uniform vec3 uColVocoder;
uniform vec3 uColOutput;
uniform vec3 uColText;
uniform vec3 uColSwirl;
uniform vec3 uColOvertone;
uniform vec3 uColRing;
uniform vec3 uColDeess;
uniform vec3 uColEnsemble;

// The creature's evolved form (blended on the CPU; the shader never branches on a stage)
uniform vec4 uEvoHead;   // cranium scale, snout, jaw mass, brow weight
uniform vec4 uEvoWing;   // span, lift, eyespots, fingers
uniform vec4 uEvoHorn;   // length, curl, second tine, crown
uniform vec4 uEvoEyes;   // brow pair open, cheek pair open, centre open, main size
uniform vec4 uEvoMaw;    // width, teeth length, third row, cheek spikes
uniform vec4 uEvoInk;    // solidity, bleed, dry brush, gloss
uniform vec4 uEvoFlow;   // drip rate, extra drips, extra tendrils, tendril reach
uniform vec4 uEvoPal;    // pastel, red-black, glow, bioluminescence
uniform vec4 uEvoMisc;   // antennae, scales, central figure, satellites
uniform vec4 uForm;      // formation, front fog opacity, fast formation, coffee ring
uniform vec4 uCloud;     // inkiness, eye spacing, crown spread, bloom phase
uniform vec4 uSeed;      // blot noise offset xy, cloud offset xy
uniform vec4 uSeedB;     // the blot being glided to xy, glide 0..1
uniform vec4 uDrift;     // cloud advection: back layer xy, front layer xy
uniform vec4 uFlow;      // slow noise scrolls: blot xy, edge bleed xy
uniform vec4 uFlow2;     // ... mist xy, veins xy
uniform vec3 uColCloud;
uniform vec3 uColCloudLit;
uniform vec3 uColUnderglow;
uniform vec3 uColRedInk;
uniform vec3 uColWashA;
uniform vec3 uColWashB;
uniform float uEgg;
uniform vec4 uLogoBox;   // centre xy, box size, unused
uniform sampler2D uLogoSdf;

float hash12 (vec2 p)
{
    vec3 p3 = fract (vec3 (p.xyx) * 0.1031);
    p3 += dot (p3, p3.yzx + 33.33);
    return fract ((p3.x + p3.y) * p3.z);
}

float vnoise (vec2 p)
{
    vec2 i = floor (p);
    vec2 f = fract (p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash12 (i);
    float b = hash12 (i + vec2 (1.0, 0.0));
    float c = hash12 (i + vec2 (0.0, 1.0));
    float d = hash12 (i + vec2 (1.0, 1.0));
    return mix (mix (a, b, u.x), mix (c, d, u.x), u.y);
}

// Value noise with its gradient (x = value, yz = d/dx, d/dy), from the same four hashes.
vec3 vnoised (vec2 p)
{
    vec2 i = floor (p);
    vec2 f = fract (p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    vec2 du = 6.0 * f * (1.0 - f);
    float a = hash12 (i);
    float b = hash12 (i + vec2 (1.0, 0.0));
    float c = hash12 (i + vec2 (0.0, 1.0));
    float d = hash12 (i + vec2 (1.0, 1.0));
    float k1 = b - a;
    float k2 = c - a;
    float k4 = a - b - c + d;
    return vec3 (a + k1 * u.x + k2 * u.y + k4 * u.x * u.y, du * vec2 (k1 + k4 * u.y, k2 + k4 * u.x));
}

float fbm (vec2 p)
{
    float s = 0.0;
    float a = 0.5;
    for (int i = 0; i < 4; i++)
    {
        s += a * vnoise (p);
        p = mat2 (1.6, 1.2, -1.2, 1.6) * p + vec2 (3.1, 1.7);
        a *= 0.5;
    }
    return s / 0.9375;
}

// A seeded lookup that follows the blot's glide (A to B), so a new seed flows in everywhere at once.
float seedNoise (vec2 p, float k)
{
    float sg = uSeedB.z;
    float n = vnoise (p + (sg > 0.999 ? uSeedB.xy : uSeed.xy) * k);
    if (sg > 0.001 && sg < 0.999)
        n = mix (n, vnoise (p + uSeedB.xy * k), sg);
    return n;
}

// One tooth's own shape for a seed: x = length (0 = a gap), y = where the tip is broken off
// (1 = whole), z = its width in its slot.
vec3 toothOf (float r)
{
    float len = r < 0.2 ? 0.0 : (r > 0.86 ? 1.7 : 0.45 + 0.9 * fract (r * 7.31));
    float cut = (r > 0.2 && r < 0.4) ? 0.5 : 1.0;
    return vec3 (len, cut, 0.6 + 0.4 * fract (r * 3.7));
}

vec3 toothVar (float k, float row)
{
    float sg = uSeedB.z;
    vec2 c = vec2 (k * 1.37 + row * 7.1, row * 3.3);
    vec3 v = toothOf (hash12 (c + (sg > 0.999 ? uSeedB.xy : uSeed.xy)));
    if (sg > 0.001 && sg < 0.999)
        v = mix (v, toothOf (hash12 (c + uSeedB.xy)), sg);
    return v;
}

float smin (float a, float b, float k)
{
    float h = clamp (0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return mix (b, a, h) - k * h * (1.0 - h);
}

float sdEllipse (vec2 p, vec2 r)
{
    r = max (r, vec2 (1.0e-4));
    float k0 = length (p / r);
    float k1 = length (p / (r * r));
    // The usual k0 (k0 - 1) / k1 estimate has a singular point at the centre (k0 / k1 depends on the
    // direction there), which the shading turns into a fan of highlights. Inside, ease the scale
    // towards the short radius so the centre is smooth; on and outside the edge it is unchanged.
    float ratio = mix (min (r.x, r.y), k0 / max (k1, 1.0e-6), smoothstep (0.0, 1.0, k0));
    return (k0 - 1.0) * ratio;
}

// One frill spike: the k-th fan sector's ridge (k may be -1, the mirror image of spike 0),
// as long as its band is loud. fs is the angle in sectors (0 at the bottom, 16 at the last ridge).
float frillSpike (float fr, float fs, float k, float band, float r0, float gravelOn)
{
    float fu = fs - (k + 0.5);
    float flen = (0.035 + 0.17 * band) * gravelOn;
    float along = clamp ((fr - r0) / max (flen, 1.0e-3), 0.0, 1.0);
    float halfW = 0.38 * (1.0 - along) + 0.02;
    float arc = fr * 1.75 / 16.0;
    return max ((abs (fu) - halfW) * arc, max (r0 - 0.04 - fr, fr - (r0 + flen)));
}

float sdTaper (vec2 p, vec2 a, vec2 b, float ra, float rb)
{
    vec2 pa = p - a;
    vec2 ba = b - a;
    float h = clamp (dot (pa, ba) / max (dot (ba, ba), 1.0e-8), 0.0, 1.0);
    return length (pa - ba * h) - mix (ra, rb, h);
}

mat2 rot (float a)
{
    float c = cos (a);
    float s = sin (a);
    return mat2 (c, s, -s, c);
}

// A painted stroke layer: noise stretched along the local flow, with bristle grain and soft layering.
float streak (vec2 c, float ang)
{
    vec2 r = rot (ang) * c;
    vec2 s = r * vec2 (1.2, 3.6);
    float v = vnoise (s);
    return v;
}

// An almond eye: x = the eye, y = its halo, z = the iris (bright centre).
vec3 almond (vec2 h, vec2 c, float ang, float ew, float eh, float aaE)
{
    vec2 e = rot (ang) * (h - c);
    float pointed = 1.0 - 0.55 * clamp (abs (e.x) / ew, 0.0, 1.0);
    float d = sdEllipse (vec2 (e.x, e.y / pointed), vec2 (ew, eh));
    float m = 1.0 - smoothstep (-aaE, aaE, d);
    float halo = exp (-max (d, 0.0) / 0.04);
    float iris = exp (-length (e / vec2 (ew * 0.8, max (eh, 0.006))) * 2.0);
    return vec3 (m, halo, iris);
}

void main()
{
    vec2 frag = vUv * uRes;
    vec2 p = (frag - 0.5 * uRes) / uRes.y;   // y up, -0.5..0.5 top to bottom
    float px = 1.0 / uRes.y;
    float t = uTime;
    float awake = 1.0 - uSleep;
    float lq = uLiquid;
    float lph = uLiquidPhase * 6.2832;
    float size = uSize;
    float heat = clamp (uRoarDrive * (0.4 + 0.6 * uRoar), 0.0, 1.0);

    // formation: eyes and maw condense first and go last, then brow and teeth, head mass, extremities
    float F = clamp (uForm.x, 0.0, 1.0);
    float fogA = uForm.y;
    float Ffast = uForm.z;
    float Fe = smoothstep (0.0, 0.35, F);
    float Fm = smoothstep (0.15, 0.6, F);
    float Fh = smoothstep (0.3, 0.8, F);
    float Fx = smoothstep (0.5, 1.0, F);
    float Fw = smoothstep (0.35, 0.95, F);   // the wings (the blot itself) come before the spikes
    float sigma = 1.0 - 0.6 * Ffast;          // softness: loud moments sharpen, quiet softens
    float inky = uCloud.x;
    float egg = uEgg;

    // ---- the card: dark scope, faint grid, a fold down the middle, paper fibre, vignette
    vec3 col = uColScope;
    vec2 gp = p + heat * awake * 0.004 * vec2 (sin (p.y * 60.0 + t * 9.0), sin (p.x * 47.0 - t * 7.0));
    vec2 gd = abs (fract (gp * 8.0 + 0.5) - 0.5) / (8.0 * px);
    // (the grid is laid on after the clouds, only in their gaps: graph paper never shows through a storm)
    float grid = 1.0 - smoothstep (0.0, 1.25, min (gd.x, gd.y));
    col += uColGrid * exp (-abs (p.x) / (2.0 * px)) * 0.5;
    col += (vnoise (frag * 0.45) - 0.5) * 0.010;
    vec2 vg = p * vec2 (0.42, 1.25);
    float vig = 1.0 - 0.45 * dot (vg, vg);
    col *= vig;

    // ---- the cloud field: not mirrored, shared by everything below
    vec2 cw = p * 1.55 + uSeed.zw + uDrift.xy;
    vec3 w1 = vnoised (cw * 0.9);
    vec3 w2 = vnoised (cw * 0.9 + vec2 (7.3, 2.9));
    vec2 warp = vec2 (w1.x, w2.x);
    // slow curling plumes (the curl of the warp noise), heavier with the liquid-ink feel
    vec2 curl = vec2 (w1.z, -w1.y);
    vec2 cwi = cw + 1.4 * warp + 0.12 * inky * curl;
    float C = 0.0;
    float R = 0.0;
    float amp = 0.5;
    vec2 cq = cwi;
    for (int i = 0; i < 4; i++)
    {
        float n = vnoise (cq);
        C += amp * n;
        // a ridged copy of the fine octaves, for feathery fingering at bloom edges
        if (i > 1)
            R += amp * (1.0 - abs (2.0 * n - 1.0));
        cq = mat2 (1.6, 1.2, -1.2, 1.6) * cq + vec2 (3.1, 1.7);
        amp *= 0.5;
    }
    C /= 0.9375;
    R /= 0.1875;
    // (a fixed stroke direction with the warp bending the strokes: rotating the whole field by a
    // spatially changing angle would turn its far-off coordinates into fine moire rings)
    float flowAng = -0.18;
    float Sb = streak (cw + 0.6 * warp, flowAng);
    float cloud = C * 0.8 + Sb * 0.2;
    // blooms spread and settle: the density threshold drifts over tens of seconds
    float theta = 0.55 - 0.05 * sin (uCloud.w + 6.2832 * w2.x);
    float nearRim = exp (-abs (cloud - theta) / 0.05);
    cloud -= 0.08 * inky * R * nearRim;

    // ---- mirror: the blot is the same on both halves
    vec2 q = vec2 (abs (p.x), p.y);

    float breath = sin (t * mix (1.6, 0.85, uSleep));
    float scale = 1.0 + clamp (-uPitch / 12.0, -1.0, 2.0) * 0.065 + 0.014 * breath * (0.35 + uSleep)
                + uJaw * 0.025 * awake;
    float shake = uGrowl * uGrowlMod * 0.006 * (0.3 + uJaw) * awake;
    vec2 h = (q - vec2 (0.0, 0.025 + shake)) / scale;
    float aa = 1.5 * px / scale;

    // liquid: the whole skin wobbles
    vec2 wob = vec2 (sin (h.y * 17.0 + t * 1.9 + lph), sin (h.x * 13.0 + h.y * 6.0 + t * 1.4 - lph * 0.5));
    h += wob * lq * 0.010;

    float cran = uEvoHead.x;
    float snout = uEvoHead.y;
    float jawM = uEvoHead.z;
    float sizeJ = size + jawM;
    // erosion while the face is still cloud (unmirrored, so the edges come apart unevenly)
    float er = 0.10 + 0.08 * cloud;
    float erH = (1.0 - Fh) * er;
    float erX = (1.0 - Fx) * er;
    float erW = (1.0 - Fw) * er;

    // ---- mouth geometry (shared by the silhouette, the maw and the teeth)
    float mY = -0.075;
    // never wider than the eyes' outer corners: a loud moment opens the mouth up and down, not sideways
    float eyeOuter = 0.135 * uCloud.y + 0.056 * mix (1.0, uEvoEyes.w, 0.5);
    float mw = min ((0.15 + 0.08 * size) * uEvoMaw.x, eyeOuter * 0.92) * (1.0 - 0.10 * uJaw * awake);
    float open = (0.006 + uJaw * (0.12 + 0.05 * sizeJ)) * awake;
    open = max (open + uGrowl * uGrowlMod * 0.014 * uJaw * awake, 0.003);
    open = max (open * (1.0 - 0.85 * uGateClosed), 0.003);   // the gate shut: the lips close
    float chinDrop = open * 0.85;
    float chinY = -0.29 - chinDrop - 0.03 * jawM;

    // ---- silhouette
    float dCran = sdEllipse (h - vec2 (0.0, 0.10 * cran), vec2 (0.29, 0.26) * cran);
    float dCheek = sdEllipse (h - vec2 (0.12 + 0.05 * sizeJ, -0.06), vec2 (0.20 + 0.05 * sizeJ, 0.15));
    float dJaw = sdTaper (h, vec2 (0.22 + 0.06 * sizeJ, -0.02), vec2 (0.09 + 0.04 * sizeJ, -0.21 - chinDrop - 0.04 * jawM),
                          0.06 + 0.02 * jawM, 0.07 + 0.02 * jawM);
    float dChin = sdEllipse (h - vec2 (0.0, -0.18 - chinDrop - 0.03 * jawM), vec2 (0.15 + 0.07 * sizeJ, 0.11 + 0.02 * jawM));

    // horns: a nub that extrudes, curls (a third segment follows the curl) and grows a second tine
    float hl = uEvoHorn.x;
    float hc = uEvoHorn.y;
    vec2 hr = vec2 (0.19, 0.22 * cran);
    vec2 m1 = hr + vec2 (0.16, 0.07) * (0.25 + 0.75 * hl);
    vec2 m2 = m1 + rot (hc * 0.9) * vec2 (0.14, 0.11) * hl;
    float curlW = smoothstep (0.0, 0.3, hc);
    vec2 m3 = m2 + rot (hc * 2.0) * vec2 (0.14, 0.11) * 0.7 * hl * curlW;
    float rb2 = mix (0.004, 0.013, curlW);
    float dHorn = min (sdTaper (h, hr, m1, 0.055, 0.032), sdTaper (h, m1, m2, 0.032, rb2));
    dHorn = min (dHorn, sdTaper (h, m2, m3, rb2, 0.003) + (1.0 - curlW) * 0.02);
    if (uEvoHorn.z > 0.001)
        dHorn = min (dHorn, sdTaper (h, m1 - vec2 (0.02, 0.0), m1 + vec2 (0.02, 0.15) * uEvoHorn.z * hl, 0.022, 0.003));
    dHorn += erX;

    float spk = uEvoMaw.w;
    vec2 s1a = vec2 (0.27 + 0.05 * sizeJ, -0.06);
    vec2 s2a = vec2 (0.24 + 0.05 * sizeJ, -0.14 - chinDrop * 0.5);
    float dSpike = min (sdTaper (h, s1a, s1a + vec2 (0.12 + 0.02 * sizeJ, -0.06) * spk, 0.03, 0.003),
                        sdTaper (h, s2a, s2a + vec2 (0.08 + 0.01 * sizeJ, -0.11 - chinDrop * 0.1) * spk, 0.025, 0.003));
    dSpike += erX + (1.0 - smoothstep (0.0, 0.2, spk)) * 0.02;

    float dHead = smin (dCran, dCheek, 0.07);
    dHead = smin (dHead, dJaw, 0.05);
    dHead = smin (dHead, dChin, 0.06);
    dHead += erH;
    dHead = smin (dHead, dHorn, 0.04);
    dHead = smin (dHead, dSpike, 0.025);

    // crown of spines: 4 mirrored + 1 on the axis, rooted in the cranium, growing one after another
    float dCrown = 1.0e3;
    if (uEvoHorn.w > 0.001)
    {
        for (int i = 0; i < 5; i++)
        {
            float fi = float (i);
            float wI = smoothstep (fi / 5.0, (fi + 1.5) / 5.0, uEvoHorn.w * 1.2);
            float ang = fi * 0.27 * uCloud.z;
            vec2 dir = vec2 (sin (ang), cos (ang));
            vec2 root = vec2 (0.0, 0.10 * cran) + dir * vec2 (0.29, 0.26) * cran * 0.9;
            float len = (0.10 + 0.06 * (1.0 - fi / 4.0)) * wI;
            dCrown = min (dCrown, sdTaper (h, root, root + dir * len, 0.022 * (0.3 + 0.7 * wI), 0.002));
        }
        dHead = smin (dHead, dCrown + erX, 0.02);
    }

    // antennae (the moth's): a curved feeler from the brow with a club at the tip
    float dAnt = 1.0e3;
    if (uEvoMisc.x > 0.001)
    {
        float an = uEvoMisc.x;
        vec2 a0 = vec2 (0.05, 0.28 * cran);
        vec2 a1 = a0 + vec2 (0.08, 0.20) * an;
        vec2 a2 = a1 + vec2 (0.17, 0.09) * an;
        dAnt = min (sdTaper (h, a0, a1, 0.011, 0.007), sdTaper (h, a1, a2, 0.007, 0.004));
        dAnt = min (dAnt, sdEllipse (rot (-0.5) * (h - a2), vec2 (0.026, 0.012) * an));
        dHead = smin (dHead, dAnt + erX, 0.015);
    }

    // the blot's wings spread sideways, swelling a little with the output
    float swell = 1.0 + 0.06 * uLevel * awake + 0.02 * breath;
    float span = uEvoWing.x;
    vec2 wq = vec2 (0.30, 0.0) + rot (-uEvoWing.y) * (h - vec2 (0.30, 0.0));
    wq.x = 0.30 + (wq.x - 0.30) / span;
    float spanK = mix (1.0, span, 0.5);
    // (an envelope cut by noise, so the wings read as pressed ink rather than shapes)
    float env = sdEllipse (rot (-0.10) * (wq - vec2 (0.80, 0.0)), vec2 (0.60, 0.25) * swell);
    vec2 bq = wq * vec2 (2.4, 3.6) + uFlow.xy + vec2 (0.0, lph * 0.03 * lq);
    float sg = uSeedB.z;
    float blotN = fbm (bq + (sg > 0.999 ? uSeedB.xy : uSeed.xy));
    if (sg > 0.001 && sg < 0.999)
        blotN = mix (blotN, fbm (bq + uSeedB.xy), sg);   // a new blot: the ink flows from one shape to the next
    float dWing = -smin (-env, (blotN - 0.50) * 0.22, 0.04);
    dWing = smin (dWing, sdEllipse (rot (-0.35) * (wq - vec2 (0.50, 0.07)), vec2 (0.21, 0.085) * swell), 0.11);
    dWing = smin (dWing, sdEllipse (rot (0.30) * (wq - vec2 (0.66, -0.17)), vec2 (0.15, 0.05) * swell), 0.10);

    // wing fingers: bone spurs from the root to the edge, the membrane scalloped between them
    float dSpur = 1.0e3;
    if (uEvoWing.w > 0.001)
    {
        float fg = uEvoWing.w;
        vec2 wr = vec2 (0.42, 0.06);
        for (int i = 0; i < 3; i++)
        {
            float fi = float (i);
            float wI = smoothstep (fi / 3.0, (fi + 1.5) / 3.0, fg * 1.2);
            vec2 tip = vec2 (1.00, 0.33);
            if (i == 1) tip = vec2 (1.43, 0.07);
            if (i == 2) tip = vec2 (1.12, -0.24);
            dSpur = min (dSpur, sdTaper (wq, wr, wr + (tip - wr) * wI, 0.020 * wI, 0.003) + (1.0 - smoothstep (0.0, 0.3, wI)) * 0.03);
        }
        dWing = max (dWing, -(length ((wq - vec2 (1.30, -0.19)) * vec2 (1.0, 1.3)) - 0.13 * fg));
        dWing = max (dWing, -(length ((wq - vec2 (1.26, 0.27)) * vec2 (1.0, 1.4)) - 0.10 * fg));
        dWing = smin (dWing, dSpur, 0.02);
    }

    // wing eyespots: pale holes with a dark pupil, a second, larger face in the wings
    if (uEvoWing.z > 0.001)
    {
        float es = sqrt (uEvoWing.z);
        float re = length ((wq - vec2 (0.86, 0.03)) / vec2 (0.11, 0.08));
        // (born at radius 0 and a little below the surface, so nothing appears as a dot)
        float born = (1.0 - smoothstep (0.0, 0.25, es)) * 0.02;
        dWing = max (dWing, -(re - es) * 0.08 - born);
        dWing = min (dWing, (re - 0.38 * es) * 0.08 + born);
    }

    float sat = 0.4 + uEvoMisc.w;
    dWing = min (dWing, length (wq - vec2 (1.32, -0.04)) - 0.035 * sat);
    dWing = min (dWing, length (wq - vec2 (1.44, 0.15)) - 0.022 * sat);
    dWing = min (dWing, length (wq - vec2 (0.96, 0.31)) - 0.030 * sat);
    dWing = min (dWing, length (wq - vec2 (1.24, -0.26)) - 0.024 * sat);
    dWing = min (dWing, length (wq - vec2 (0.50, 0.36)) - 0.018 * sat);
    dWing = dWing * spanK + erW;

    float dInk = smin (dHead, dWing + 0.010, 0.12);

    // the pressed blot's central figure on the fold: a narrow body, knobbed at the top, a pelvis below
    float dFig = 1.0e3;
    if (uEvoMisc.z > 0.001)
    {
        float cf = uEvoMisc.z;
        dFig = sdTaper (h, vec2 (0.0, 0.43 * cf + 0.10 * (1.0 - cf)), vec2 (0.0, -0.42 * cf - 0.10 * (1.0 - cf)), 0.022 * cf, 0.03 * cf);
        dFig = smin (dFig, sdEllipse (h - vec2 (0.0, -0.33), vec2 (0.085, 0.12) * cf), 0.04);
        dFig = min (dFig, length (h - vec2 (0.055, 0.47)) - 0.028 * cf);
        dInk = smin (dInk, dFig + erX + (1.0 - smoothstep (0.0, 0.2, cf)) * 0.03, 0.03);
    }

    // ink tendrils dripping off the underside, longer with liquid and with the liquid forms
    float dripAmt = smoothstep (0.03, 0.45, lq + 0.12 * max (uEvoFlow.x - 1.0, 0.0));
    float dDrip = 1.0e3;
    for (int i = 0; i < 7; i++)
    {
        float fi = float (i);
        float x = 0.06;
        float top = -0.27 - chinDrop;
        float wI = 1.0;
        if (i == 1) { x = 0.25; top = -0.22 - chinDrop * 0.6; }
        if (i == 2) { x = 0.56; top = -0.03; }
        if (i == 3) { x = 0.86; top = -0.14; }
        if (i == 4) { x = 0.40; top = -0.10; }
        if (i == 5) { x = 0.72; top = -0.12; }
        if (i == 6) { x = 1.05; top = -0.10; }
        if (i > 3)
        {
            if (uEvoFlow.y < 0.001)
                break;
            wI = smoothstep ((fi - 4.0) / 3.0, (fi - 2.5) / 3.0, uEvoFlow.y * 1.2);
        }
        float dr = dripAmt * wI;
        // (the rate never scales the clock: a phase that depends on the form would jump as it evolves)
        float life = fract (t * (0.06 + 0.035 * fi) + fi * 0.37);
        float len = (0.02 + 0.17 * life) * dr * (0.8 + 0.2 * min (uEvoFlow.x, 2.5));
        vec2 tip = vec2 (x + 0.008 * sin (t * 1.3 + fi * 2.0), top - len);
        float d = sdTaper (h, vec2 (x, top + 0.03), tip, 0.018 * dr, (0.005 + 0.006 * life) * dr);
        d = min (d, length (h - tip) - (0.008 + 0.010 * life) * dr);
        float fall = fract (t * 0.33 + fi * 0.61);
        vec2 drop = vec2 (tip.x, top - 0.19 * dr - fall * 0.5);
        d = min (d, length ((h - drop) * vec2 (1.0, 0.8)) - 0.010 * dr * (1.0 - fall));
        dDrip = min (dDrip, d);
    }
    dInk = smin (dInk, dDrip + erX, 0.03);

    // ensemble: ink filaments reaching out to the sides, as far as the chorus is wide; the liquid
    // forms keep some reach of their own (and grow more of them)
    float reach = uEvoFlow.w;
    float reachInk = smoothstep (0.25, 0.7, reach);
    float tl = 0.05 + 0.85 * clamp (uEnsWidth + reach * 0.6, 0.0, 1.0);
    float dTend = 1.0e3;
    for (int i = 0; i < 6; i++)
    {
        float fi = float (i);
        float y0 = 0.03 - 0.09 * fi;
        float x0 = 0.27 + 0.03 * fi;
        float wI = 1.0;
        if (i == 3) { y0 = 0.10; x0 = 0.28; }
        if (i == 4) { y0 = -0.12; x0 = 0.34; }
        if (i == 5) { y0 = 0.17; x0 = 0.25; }
        if (i > 2)
        {
            if (uEvoFlow.z < 0.001)
                break;
            wI = smoothstep ((fi - 3.0) / 3.0, (fi - 1.5) / 3.0, uEvoFlow.z * 1.2);
        }
        float tli = tl * wI;
        float u = clamp ((h.x - x0) / max (tli, 1.0e-3), 0.0, 1.0);
        float yy = y0 + (0.045 * sin (h.x * (11.0 + 3.0 * fi) - t * (0.9 + 0.3 * fi) + fi * 2.1)
                         + 0.02 * sin (h.x * 27.0 + t * 1.3 + fi)) * u - 0.07 * u * u * (fi - 0.6 - (i > 2 ? 3.0 : 0.0))
                 - 0.16 * u * u * reachInk;   // ink arms hang heavier than the chorus' light filaments
        float thick = mix (0.006 + 0.008 * reachInk, 0.0008 + 0.002 * reachInk, u);
        // fades along its length, so it reads as ink thinning out rather than a drawn line
        float dd = max (abs (h.y - yy) - thick, max (x0 - h.x, h.x - (x0 + tli)));
        // and grows out of the cheek instead of starting with a squared-off end
        dd += 0.010 * (1.0 - smoothstep (0.0, 0.06, h.x - x0));
        dTend = min (dTend, dd + 0.02 * u * u + (1.0 - wI) * 0.05);
    }
    float dBody = dInk;   // (the shading's height field leaves the thin arms flat)
    if (reachInk > 0.001)
        dInk = smin (dInk, dTend + erX + (1.0 - reachInk) * 0.03, 0.02);

    // vocoder frill: a fan of 32 ridges under the jaw (even bands on the left, odd on the right,
    // low bands in the middle), each spike as long as its band is loud
    vec2 fc = vec2 (0.0, -0.13 - chinDrop * 0.6 - 0.03 * jawM);
    vec2 fv = h - fc;
    float fr = length (fv);
    float fang = atan (fv.x, -fv.y);
    float sector = clamp (fang / 1.75, 0.0, 0.9999) * 16.0;
    float fk = floor (sector);
    float fu = fract (sector) - 0.5;
    // clamped as a float (no integer clamp in 1.10) so a degenerate angle can't index out of range
    float bandSlot = clamp (fk * 2.0 + (p.x > 0.0 ? 1.0 : 0.0), 0.0, 31.0);
    float band = clamp (uBands[int (bandSlot)], 0.0, 1.0);
    float r0 = 0.17 + 0.04 * sizeJ;
    float flen = (0.035 + 0.17 * band) * uGravelOn;
    float along = clamp ((fr - r0) / max (flen, 1.0e-3), 0.0, 1.0);
    float halfW = 0.38 * (1.0 - along) + 0.02;
    // The distance is the nearer of this sector's spike and the neighbouring one, so it is continuous
    // across sector edges (one spike alone jumps there, and the jump shows as wedges in the shading).
    float fs = fang * 16.0 / 1.75;
    float side = p.x > 0.0 ? 1.0 : 0.0;
    float fn = fk + (fs > fk + 0.5 ? 1.0 : -1.0);
    float bandSlotN = fn < 0.0 ? 1.0 - side : clamp (fn * 2.0 + side, 0.0, 31.0);
    float bandN = clamp (uBands[int (bandSlotN)], 0.0, 1.0);
    float dFrill = frillSpike (fr, fs, fk, band, r0, uGravelOn);
    dFrill = fn > 15.5 ? dFrill : min (dFrill, frillSpike (fr, fs, fn, bandN, r0, uGravelOn));
    dFrill += (1.0 - uGravelOn) * 0.05 + erH;
    dInk = smin (dInk, dFrill, 0.015);
    dBody = smin (dBody, dFrill, 0.015);

    // the hidden logo: the ink flows into it from the centre outward, broken up by the clouds
    float gl = 0.0;
    float dLogoC = 1.0e3;
    if (egg > 0.001)
    {
        vec2 luv = (p - uLogoBox.xy) / uLogoBox.z + 0.5;
        vec4 L = texture2D (uLogoSdf, clamp (luv, 0.0, 1.0));
        float boxD = length (max (abs (luv - 0.5) - 0.5, 0.0)) * uLogoBox.z;
        float dLogoG = (L.r - 0.5) * 0.25 * uLogoBox.z + boxD;
        dLogoC = (L.g - 0.5) * 0.25 * uLogoBox.z + boxD;
        gl = clamp (egg * 1.6 - 0.3 + 0.3 * (cloud - 0.5) - 0.25 * length (p), 0.0, 1.0);
        // (capped outside: far from the logo the ink thins at the pace of the morph, not at the pace of the distance)
        float dl = min (min (dLogoG, dLogoC), 0.12);
        float swellG = 0.03 * sin (3.1416 * gl);
        dInk = mix (dInk, dl, gl) - swellG;
        dBody = mix (dBody, dl, gl) - swellG;
        dHead = mix (dHead, dl, gl) - swellG;
    }

    // inky edges: slow-moving noise bleeds the outline, more so out on the wings and with liquid
    vec2 eq = h * 6.0 + uFlow.zw;
    float edgeN = fbm (eq + (sg > 0.999 ? uSeedB.xy : uSeed.xy) * 0.5);
    if (sg > 0.001 && sg < 0.999)
        edgeN = mix (edgeN, fbm (eq + uSeedB.xy * 0.5), sg);
    float bleedK = uEvoInk.y;
    float bleedD = (edgeN - 0.5) * (0.020 + 0.030 * lq + 0.045 * smoothstep (0.35, 1.3, h.x)) * bleedK;
    dInk += bleedD;
    dBody += bleedD;
    // capillary feathering for the wettest blots
    dInk += (vnoise (h * 45.0) - 0.5) * 0.010 * max (bleedK - 1.0, 0.0);
    // the noisy parts are not true distances: rescale by the gradient so edges and halos keep their width
    // (only near the edge: inside, the per-quad gradient would show as steps in the shading)
    float grad = length (vec2 (dFdx (dInk), dFdy (dInk))) * scale / px;
    float dEdge = dInk / mix (1.0, clamp (grad, 0.25, 1.5), smoothstep (-0.04, -0.01, dInk));
    float bleed = aa + 0.002 * edgeN;

    // ---- occupancy: the body is condensed cloud; its edge band widens as the formation drops
    // (and while the ink flows into the logo and back: the moving outline stays soft, a flow, not a wipe)
    float wB = bleed + 0.003 * sigma + 0.05 * (1.0 - F) + 0.06 * sin (3.1416 * min (egg * 1.25, 1.0));
    float occ = 1.0 - smoothstep (-wB, wB, dEdge);
    // edges dissolve into the cloud field
    occ *= smoothstep (0.15, 0.55, cloud + 0.45 * occ + 0.25 * F);
    // the lower face trails off into mist
    float lowFace = smoothstep (chinY + 0.06, chinY - 0.20, h.y) * (1.0 - smoothstep (0.18, 0.55, h.x)) * (1.0 - gl);
    float mistN = vnoise (vec2 (p.x * 8.0 + 0.3 * warp.y, p.y * 1.3 + uDrift.y * 4.0) - uFlow2.xy);
    occ *= 1.0 - lowFace * (0.45 + 0.5 * mistN);
    float ink = occ;

    // ---- height field for wet-ink shading (normals from screen derivatives)
    float H = 0.55 * (1.0 - exp (-max (-dBody, 0.0) * 9.0)) + 0.35 * (1.0 - exp (-max (-dHead, 0.0) * 5.0));
    float browW = uEvoHead.w * Fm;
    float dBrow = sdTaper (h, vec2 (0.03, 0.118), vec2 (0.25, 0.205), 0.018, 0.030);
    float dBrow2 = sdTaper (h, vec2 (0.06, 0.185), vec2 (0.21, 0.265), 0.012, 0.020);
    float dCheekBone = sdTaper (h, vec2 (0.15, -0.005), vec2 (0.28, 0.035), 0.016, 0.010);
    float dNose = sdTaper (h, vec2 (0.0, 0.12), vec2 (0.0, -0.01 * (1.0 + snout)), 0.022, 0.032 + 0.012 * snout);
    H += (0.38 * smoothstep (0.02, -0.015, dBrow) + 0.22 * smoothstep (0.015, -0.01, dBrow2)) * browW;
    H += (0.18 * smoothstep (0.02, -0.01, dCheekBone) + 0.15 * smoothstep (0.02, -0.01, dNose)) * Fm;
    // the muzzle of the long-faced forms
    H += 0.28 * snout * smoothstep (0.03, -0.04, sdEllipse (h - vec2 (0.0, -0.035), vec2 (0.08 + 0.05 * snout, 0.06 + 0.03 * snout))) * Fh;
    float dNostril = sdEllipse (rot (0.5) * (h - vec2 (0.034 + 0.01 * snout, -0.012 - 0.01 * snout)), vec2 (0.014, 0.006) * (1.0 + 0.4 * snout));
    H -= 0.25 * smoothstep (0.006, -0.004, dNostril) * Fm;
    // wing fingers and the central figure stand up from the ink
    H += 0.12 * uEvoWing.w * smoothstep (0.02, -0.01, dSpur) * Fx;
    H += 0.25 * smoothstep (0.015, -0.01, dFig);
    // skin: soft lumps for the light, thin veins only as colour (as bumps they alias into outlines)
    // (rotated and warped: a plain value-noise contour on the grid draws rounded boxes, not veins)
    vec2 vp = rot (0.6) * h * 16.0 + uFlow2.zw + vec2 (0.0, lph * 0.15);
    vp += 1.6 * vec2 (vnoise (h * 7.0 + vec2 (5.2, 1.3)), vnoise (h * 7.0 + vec2 (2.8, 9.1)));
    float veinN = abs (vnoise (vp) - 0.5);
    float veins = (1.0 - smoothstep (0.0, 0.06, veinN)) * smoothstep (0.0, -0.04, dInk);
    H += (vnoise (h * 22.0) - 0.5) * (0.04 + 0.06 * uDetailHigh + 0.06 * uCut);
    float ripple = sin (length (h - vec2 (0.0, 0.05)) * 70.0 - t * 4.0 - lph);
    H += lq * 0.05 * ripple * smoothstep (0.0, -0.05, dInk);
    // the body keeps the texture of the cloud it is made of
    H += 0.06 * (C - 0.5) * occ;
    // scales on the armoured forms
    if (uEvoMisc.y > 0.001)
    {
        vec2 su = h * vec2 (30.0, 40.0);
        su.x += 0.5 * mod (floor (su.y), 2.0);
        su += 0.35 * vec2 (vnoise (h * 9.0 + 2.3), vnoise (h * 9.0 + 7.9));
        float sb = 1.0 - smoothstep (0.15, 0.55, length (fract (su) - vec2 (0.5, 0.35)));
        // only on the brow, the cheeks and the temples (never as an even grid over the face)
        float zone = smoothstep (0.35, 0.75, seedNoise (h * 5.0, 0.7)) * smoothstep (0.05, 0.16, abs (h.y - mY + 0.01));
        H += 0.035 * smoothstep (0.3, 1.0, uEvoMisc.y) * sb * zone * smoothstep (0.0, -0.04, dHead) * Fh;
    }

    // eyes: slanted almonds under the brows, sockets sunk into the skin
    float eSize = uEvoEyes.w;
    vec2 ec = vec2 (0.135 * uCloud.y, 0.083);
    vec2 e = rot (-0.38) * (h - ec);
    float ew = 0.060 * mix (1.0, eSize, 0.5);
    float eyeOpen = awake * (1.0 - uBlink);
    float roundness = smoothstep (1.0, 1.15, eSize);
    float eh = 0.032 * clamp ((eSize - 0.55) / 0.45, 0.12, 1.4) * (1.0 + 0.5 * roundness) * eyeOpen + 0.0012;
    float pointed = 1.0 - 0.55 * (1.0 - roundness) * clamp (abs (e.x) / ew, 0.0, 1.0);
    float dEye = sdEllipse (vec2 (e.x, e.y / pointed), vec2 (ew, eh));
    float dSocket = sdEllipse (e - vec2 (0.0, -0.004), vec2 (ew * 1.3, 0.045));
    H -= 0.32 * smoothstep (0.02, -0.02, dSocket) * Fm;

    // more eyes open on the stranger forms: a crease first, then a slit of glow, then the lid opens
    vec3 eyB = vec3 (0.0);
    vec3 eyK = vec3 (0.0);
    vec3 eyC = vec3 (0.0);
    vec3 openX = clamp (uEvoEyes.xyz, 0.0, 1.0);
    float aaE = aa + 0.006 * sigma;
    if (openX.x + openX.y + openX.z > 0.001)
    {
        vec3 lid = smoothstep (0.4, 1.0, openX) * eyeOpen;
        vec2 cB = vec2 (0.075 * uCloud.y, 0.205 * cran);
        vec2 cK = vec2 (0.215 * uCloud.y, 0.005);
        vec2 cC = vec2 (0.0, 0.25 * cran);
        eyB = almond (h, cB, -0.25, 0.030, 0.016 * lid.x + 0.001, aaE);
        eyK = almond (h, cK, -0.55, 0.026, 0.012 * lid.y + 0.001, aaE);
        eyC = almond (h, cC, 1.5708, 0.034, 0.014 * lid.z + 0.001, aaE);
        vec3 crease = smoothstep (0.0, 0.15, openX);
        H -= 0.20 * crease.x * exp (-dot ((h - cB) / vec2 (0.045, 0.025), (h - cB) / vec2 (0.045, 0.025))) * Fm;
        H -= 0.20 * crease.y * exp (-dot ((h - cK) / vec2 (0.040, 0.022), (h - cK) / vec2 (0.040, 0.022))) * Fm;
        H -= 0.20 * crease.z * exp (-dot ((h - cC) / vec2 (0.022, 0.050), (h - cC) / vec2 (0.022, 0.050))) * Fm;
    }

    // maw
    // a ragged slot, not a crescent: the corners turn down, the upper lip lifts off the teeth as the
    // jaw drops, and the lip edge is the cloud's own (unmirrored, so the two sides differ a little)
    float mx = h.x / mw;
    float mx2 = mx * mx;
    float lipCurve = sqrt (max (1.0 - mx2, 0.0));
    float rag = (C - 0.5) * 0.014 * (0.5 + 0.5 * sigma) + (vnoise (vec2 (h.x * 34.0, 3.0)) - 0.5) * 0.008;
    float rag2 = (vnoise (vec2 (h.x * 29.0, 8.0)) - 0.5) * 0.010 - (C - 0.5) * 0.010 * sigma;
    float yU = mY + 0.012 * (1.0 - mx2) - 0.020 * mx2 + open * (0.22 * lipCurve - 0.25 * mx2) + rag * (0.4 + 0.6 * lipCurve);
    float yL = mY - 0.020 * mx2 - 0.002 * (1.0 - mx2) - open * (0.78 * lipCurve + 0.25 * mx2) + rag2 * (0.3 + 0.7 * lipCurve);
    float mouthMid = mY - open * 0.28;
    float dMouth = max (h.x - mw, max (h.y - yU, yL - h.y));
    float dLips = sdEllipse ((h - vec2 (0.0, mouthMid)) / vec2 (1.0, 1.0 + open * 6.0), vec2 (mw * 1.08, 0.03));
    H += 0.12 * smoothstep (0.015, -0.01, dLips) * Fm;
    H *= 1.0 - 0.6 * gl;

    vec2 dH = vec2 (dFdx (H), dFdy (H)) / (px * 2.0);
    vec3 n = normalize (vec3 (-dH.x * 0.03, -dH.y * 0.03, 1.0));
    vec3 L = normalize (vec3 (0.0, 0.55, 0.85));
    float diff = max (dot (n, L), 0.0);
    vec3 halfV = normalize (L + vec3 (0.0, 0.0, 1.0));
    float specFlat = pow (halfV.z, 30.0);   // flat ink gets no highlight, only the ridges and swells do
    float spec = max (pow (max (dot (n, halfV), 0.0), 30.0) - specFlat, 0.0) / (1.0 - specFlat);

    // ---- the clouds behind (a little denser where the face would be: a shape you half see)
    float dens = smoothstep (theta - 0.12, theta + 0.20, cloud);
    // top light from the screen-space slope of the density (upper edges catch the light)
    float slope = clamp (dFdy (cloud) / px * 0.03, -1.0, 1.0);
    float lit = clamp (0.20 + 0.70 * pow (dens, 0.7) * (0.55 - 0.75 * slope) + 0.6 * (cloud - theta) + 0.25 * (Sb - 0.5), 0.0, 1.0);
    vec3 cloudCol = mix (uColCloud, uColCloudLit, lit);
    // translucent washes: where the two stroke layers overlap the colour deepens
    vec2 cwF = p * 1.55 + uSeed.zw * 1.7 + uDrift.zw + vec2 (11.3, 4.7);
    float Sf = streak (cwF + 0.6 * warp, flowAng * 0.8 + 0.12);
    float lumC = dot (cloudCol, vec3 (0.3, 0.59, 0.11));
    vec3 deep = mix (vec3 (lumC), cloudCol, 1.4) * 0.75;
    cloudCol = mix (cloudCol, deep, clamp (inky * 1.6 * Sb * Sf, 0.0, 0.3));
    // sharp darker rims just inside each bloom (layered), and pale lace where the pigment is pushed apart
    float rimW = max (0.02, 1.5 * fwidth (cloud));
    float inB1 = cloud - theta;
    float inB2 = cloud - theta - 0.13;
    float rims = exp (-abs (inB1) / rimW) * smoothstep (-0.01, 0.01, inB1) + 0.6 * exp (-abs (inB2) / rimW) * smoothstep (-0.01, 0.01, inB2);
    vec3 rimCol = mix (vec3 (lumC), cloudCol, 1.2) * 0.5;
    cloudCol = mix (cloudCol, rimCol, clamp (rims * (0.20 + 0.40 * inky), 0.0, 0.34));
    // the underglow comes after the washes and the rims (they deepen the cloud's own colour, never
    // the heat's), a little warmer when loud, and kept off the rims themselves
    cloudCol += uColUnderglow * (0.10 + 0.25 * heat * awake) * (1.0 - lit) * dens * 0.4 * (1.0 - 0.6 * clamp (rims, 0.0, 1.0));
    {
        vec2 vc = cwi * 2.6;
        vec2 vi = floor (vc - 0.5);
        float f1 = 9.0;
        float f2 = 9.0;
        for (int k = 0; k < 4; k++)
        {
            vec2 o = vec2 (mod (float (k), 2.0), floor (float (k) * 0.5));
            vec2 cell = vi + o;
            vec2 fp = cell + 0.25 + 0.5 * vec2 (hash12 (cell + 0.7), hash12 (cell + 4.1));
            float dd = length (vc - fp);
            f2 = dd < f1 ? f1 : min (f2, dd);
            f1 = min (f1, dd);
        }
        float lace = (1.0 - smoothstep (0.0, 0.05, f2 - f1)) * (0.5 + 0.5 * R) * smoothstep (theta + 0.04, theta - 0.12, cloud) * smoothstep (theta - 0.30, theta - 0.12, cloud);
        cloudCol = mix (cloudCol, uColCloudLit, lace * inky * 0.5);
        dens = max (dens, lace * inky * 0.35);
    }
    float faceHint = exp (-max (dInk, 0.0) * 6.0);
    float amb = dens * (0.62 + 0.25 * faceHint * (1.0 - F));
    col = mix (col, cloudCol, amb);
    col = mix (col, uColGrid * vig, grid * 0.45 * (1.0 - smoothstep (0.0, 0.6, dens)));

    // ---- glows behind the body: sub rings, embers, the rim halo
    vec2 mc = vec2 (0.0, mouthMid * scale + 0.025);
    float rr = length ((p - mc) * vec2 (0.72, 1.0));
    float rings = 0.0;
    for (int i = 0; i < 3; i++)
    {
        float ph = fract (uSubPhase + float (i) / 3.0);
        float rad = 0.16 + ph * 1.35;
        float wdt = 0.006 + 0.035 * ph;
        float x = (rr - rad + 0.01 * sin (atan (p.y - mc.y, abs (p.x)) * 9.0 + t)) / wdt;
        rings += (1.0 - ph) * (1.0 - ph) * exp (-x * x);
    }
    float subAmt = uSub * (0.12 + 1.3 * uSubLevel) * (0.25 + 0.75 * awake);
    col += uColSub * rings * subAmt * 0.55;

    vec2 ep = vec2 (q.x, p.y) * 9.0;
    ep.y -= t * (0.5 + 1.4 * uRoarDrive);
    vec2 ecell = floor (ep);
    vec2 fl = fract (ep) - 0.5;
    float rnd = hash12 (ecell);
    vec2 off = vec2 (hash12 (ecell + 7.1), hash12 (ecell + 3.3)) - 0.5;
    off.x += 0.22 * sin (t * 2.0 + rnd * 6.28);
    float de = length (fl - off * 0.6);
    float embers = step (0.70, rnd) * exp (-de * de * 380.0) * (0.55 + 0.45 * sin (t * 6.0 + rnd * 40.0));
    embers *= smoothstep (1.25, 0.15, length (p * vec2 (0.55, 1.0)));
    col += uColRoar * embers * heat * 2.2 * awake;

    float glowK = uEvoPal.z;
    vec3 rim = mix (uColPitch, uColLiquid, smoothstep (0.0, 0.8, lq));
    rim = mix (rim, uColRoar, heat * 0.15);   // (a hint of warmth when loud: the eyes and the throat carry the heat)
    // (loudness brightens the outline only a little: at a roar it must not turn into embossed contours)
    float haloAmt = (0.08 + 0.10 * uLevel * awake + 0.05 * heat) * mix (0.7, 1.0, min (glowK, 1.0)) * Fh * (1.0 - 0.5 * gl) + 0.05 * gl;
    float halo = exp (-max (dEdge, 0.0) * 34.0) * (1.0 - ink) * (1.0 - 0.6 * smoothstep (0.4, 1.2, h.x));
    col += rim * halo * haloAmt;

    // wisps drawn off the edges by the wind
    float wisp = exp (-max (dEdge, 0.0) / 0.035) * smoothstep (0.0, 0.004, dEdge) * smoothstep (0.35, 0.8, Sb) * Fh;
    col = mix (col, mix (cloudCol, uColCloudLit, 0.3), wisp * (0.25 + 0.35 * (1.0 - F)));

    // ---- the body: cloud lit from above, turning to ink as it condenses
    vec3 bodyCloud = mix (uColCloud * 0.75, uColCloudLit * 0.9, clamp (0.2 + 0.55 * diff * diff + 0.5 * (C - 0.5), 0.0, 1.0));
    bodyCloud += uColUnderglow * (0.08 + 0.7 * heat * awake) * (1.0 - diff) * 0.5;
    float solid = ink * smoothstep (0.15, 0.85, F) * (0.25 + 0.85 * uEvoInk.x);
    // pressed ink pools at its boundary (the darkest band is just inside the edge)
    solid = clamp (solid + uForm.w * 0.45 * exp (min (dEdge, 0.0) / 0.014) * ink * F, 0.0, 1.0);
    // dry brush where the ink is thin: out on the wings and along the edge
    float thin = max (smoothstep (0.5, 1.35, h.x), exp (min (dEdge, 0.0) / 0.025) * 0.6);
    if (uEvoInk.z > 0.001)
    {
        // streaks along the (tilted) stroke, bent by the cloud, and faded out before they get finer
        // than a few pixels (fine axis-aligned streaks alias into scanlines)
        vec2 dq = rot (flowAng) * (h + 0.03 * warp);
        vec2 du = vec2 (dq.x * 5.0, dq.y * 60.0);
        float dryLim = 1.0 - smoothstep (0.3, 0.6, fwidth (du.y));
        float dryN = seedNoise (du, 1.0);
        solid *= 1.0 - uEvoInk.z * thin * smoothstep (0.45, 0.8, dryN) * 0.7 * dryLim;
    }
    // the logo is condensed cloud too: a lit, half-solid body so it reads on the dark card
    solid = mix (solid, ink * 0.5, gl);

    float density = mix (1.0, 0.90 + 0.10 * edgeN, smoothstep (0.35, 0.9, h.x));
    vec3 inkCol = vec3 (0.020, 0.022, 0.034) + uColPitch * 0.035 * edgeN;
    inkCol += uColLiquid * veins * (0.008 + 0.025 * lq + 0.02 * uDetailHigh) + uColRoar * veins * heat * 0.05;
    inkCol *= 0.45 + 1.6 * diff * diff;
    // (a loud moment dulls the sheen: the roaring creature is storm cloud, not polished)
    inkCol += mix (uColLiquid, uColText, 0.6) * spec * (0.18 + 0.40 * lq) * (uEvoInk.w / 0.5) * (1.0 - 0.65 * heat);
    float rimIn = smoothstep (-0.03, 0.0, dEdge);
    inkCol += rim * rimIn * (0.06 + 0.07 * uLevel * awake) * Fh;
    float grain = hash12 (floor (frag) + floor (t * 24.0)) - 0.5;
    inkCol += grain * (0.012 + 0.05 * uBoost + 0.025 * uDetailLow);
    float speck = step (0.9985 - 0.004 * uBoost - 0.001 * uDetailLow, hash12 (floor (frag * 0.5) + 17.0));
    inkCol += uColDetail * speck * (0.10 + 0.4 * uBoost) * smoothstep (0.0, -0.02, dInk) * solid;
    vec3 bodyCol = mix (bodyCloud, inkCol, solid);
    col = mix (col, bodyCol, ink * density);

    // pastel washes over the wings, cheeks and crown (transparent, as on the coloured plates)
    if (uEvoPal.x > 0.001)
    {
        float wA = smoothstep (0.35, 0.75, seedNoise (h * 2.2, 0.3)) * smoothstep (0.35, 0.7, h.x);
        float wB = exp (-dot ((h - vec2 (0.20, -0.05)) / vec2 (0.12, 0.08), (h - vec2 (0.20, -0.05)) / vec2 (0.12, 0.08)))
                 + exp (-dot ((h - vec2 (0.0, 0.32)) / vec2 (0.22, 0.08), (h - vec2 (0.0, 0.32)) / vec2 (0.22, 0.08)));
        float lumB = dot (col, vec3 (0.3, 0.59, 0.11));
        vec3 washed = mix (col, uColWashA * (0.22 + 1.1 * lumB), wA * 0.8);
        washed = mix (washed, uColWashB * (0.22 + 1.1 * lumB), clamp (wB, 0.0, 1.0) * 0.7);
        col = mix (col, washed, uEvoPal.x * 0.55 * max (ink, dens * 0.3) * (1.0 - gl));
    }

    // red ink: splashes on the head, under the jaw and on the wings, drawn through the black
    if (uEvoPal.y > 0.001 || egg > 0.001)
    {
        // (the crown splash and the chin stain sit inside the head, the third on the wing)
        float dRed = sdEllipse (rot (0.5) * (h - vec2 (0.13, 0.34 * cran)), vec2 (0.09, 0.04));
        dRed = smin (dRed, sdEllipse (h - vec2 (0.0, chinY + 0.07), vec2 (0.12, 0.04)), 0.04);
        dRed = min (dRed, sdEllipse (rot (-0.25) * (wq - vec2 (0.66, -0.12)), vec2 (0.13, 0.06)));
        // stained, not drawn: a ragged edge, a wet core, pooled darker at the rim, soft while the cloud is
        float rn = seedNoise (h * 14.0, 1.0) - 0.5;
        dRed += (edgeN - 0.5) * 0.07 + rn * 0.03;
        float redCore = 1.0 - smoothstep (-0.03, 0.012 + 0.02 * sigma, dRed);
        float redRim = exp (-abs (dRed + 0.006) / (0.008 + 0.006 * sigma));
        // only where there is a body to stain: never a shape floating on its own over the cloud
        float redM = clamp (redCore * (0.45 + 0.25 * C) + redRim * 0.25, 0.0, 1.0) * smoothstep (0.05, 0.6, uEvoPal.y) * Fh * (1.0 - gl)
                   * smoothstep (0.1, 0.6, ink);
        // the cherry is cloud too: soft-edged, thinner where the cloud is, lit like the body
        float logoRed = (1.0 - smoothstep (-0.012, 0.006, dLogoC)) * smoothstep (0.4, 1.0, gl);
        logoRed *= 0.6 + 0.4 * smoothstep (0.25, 0.75, cloud);
        // drawn through the ink and through the cloud body alike (multiply: it keeps their light)
        vec3 stain = mix (col * mix (vec3 (1.0), uColRedInk * 2.2, 0.85), col * mix (vec3 (1.0), uColRedInk * 2.4, 0.6), 1.0 - solid);
        col = mix (col, stain, redM);
        // a rounded body of its own (a dome over the red's distance field), lit from above like the head
        // (gentle: the field is 8-bit, a steep dome would show its steps as rings)
        float dome = smoothstep (-0.01, 0.13, -dLogoC);
        vec2 dD = vec2 (dFdx (dome), dFdy (dome)) / px;
        vec3 nC = normalize (vec3 (-dD.x * 0.012, -dD.y * 0.012, 1.0));
        float diffC = max (dot (nC, L), 0.0);
        float specC = pow (max (dot (nC, halfV), 0.0), 24.0);
        vec3 cherry = uColRedInk * (0.18 + 0.95 * diffC * diffC + 0.5 * (C - 0.5) + 0.15 * dome);
        cherry += vec3 (1.0, 0.8, 0.8) * specC * 0.18 * smoothstep (0.2, 0.8, dome);
        col = mix (col, cherry, logoRed * 0.85);
    }

    // tame: the crown and the brow dim while the top end is held back
    col *= 1.0 - 0.35 * uTame * smoothstep (0.12, 0.28, h.y) * ink;

    // swirl: a band of iridescent sheen sweeping up and down the face with the phaser's notch
    if (uSwirlAmt > 0.001)
    {
        float sy = mix (-0.30, 0.32, uSwirlNotch);
        float sband = exp (-pow ((h.y - sy) / 0.045, 2.0)) * ink;
        vec3 irid = 0.5 + 0.5 * cos (6.2832 * (vec3 (0.0, 0.33, 0.67) + h.x * 2.5 + h.y * 1.5 + t * 0.15));
        col += mix (uColSwirl, irid, 0.5) * sband * uSwirlAmt * (0.10 + 0.6 * spec + 0.25 * diff) * awake * Fh;
    }

    // the limiter straining: veins swell and pulse at the temples (and at the crown's roots)
    vec2 tc = (h - vec2 (0.225, 0.165) * vec2 (1.0, cran)) / vec2 (0.09, 0.075);
    float templeMask = exp (-dot (tc, tc)) + 0.6 * uEvoHorn.w * smoothstep (0.03, -0.01, dCrown) * smoothstep (0.30, 0.20, length (h - vec2 (0.0, 0.10 * cran)) - 0.12);
    if (uStrain > 0.001)
    {
        vec2 tvp = rot (1.1) * h * 30.0 + 1.3 * vec2 (vnoise (h * 11.0 + 3.7), vnoise (h * 11.0 + 8.2));
        float tvein = 1.0 - smoothstep (0.0, 0.07, abs (vnoise (tvp) - 0.5));
        col += mix (uColOutput, uColRoar, 0.35) * (tvein * 2.2 + 0.25) * templeMask * ink * uStrain * (0.25 + 1.2 * uStrainPulse) * awake * Fh;
    }

    // ensemble filaments (over the ink, in the ensemble's colour)
    float tendA = smoothstep (0.02, 0.25, uEnsWidth) * awake * Fx;
    col += uColEnsemble * (exp (-max (dTend, 0.0) / 0.0025) * 0.35 + exp (-max (dTend, 0.0) / 0.02) * 0.08) * tendA;

    // overtone: one glowing rib per harmonic down the throat and chest; Replace hollows the chest
    float ovA = uOvLevel * awake * Fh * (1.0 - gl);
    vec2 cc = vec2 (h.x / 0.15, (h.y + 0.28 + chinDrop + 0.03 * jawM) / 0.09);
    col *= 1.0 - 0.65 * uOvReplace * ovA * exp (-dot (cc, cc));
    float ribs = 0.0;
    float ribCore = 0.0;
    for (int k = 0; k < 5; k++)
    {
        float fkk = float (k);
        float hk = clamp (uOvHarm[k], 0.0, 1.0);
        float yk = -0.215 - chinDrop - 0.03 * jawM - 0.032 * fkk;
        float xr = h.x / (0.11 - 0.012 * fkk);
        float d = abs (h.y - (yk + 0.035 * xr * xr));
        float inside = 1.0 - smoothstep (0.8, 1.0, xr);
        float flick = 0.75 + 0.25 * sin (t * (9.0 + 3.0 * fkk) + fkk * 1.7);
        ribCore += exp (-d / 0.0022) * inside * hk * flick;
        ribs += exp (-d / 0.014) * inside * hk * flick;
    }
    ovA = clamp (ovA * 1.6, 0.0, 1.0);
    col += uColOvertone * ribs * ovA * mix (0.45, 0.22, uOvReplace) + mix (uColOvertone, vec3 (1.0), 0.3) * ribCore * ovA * 1.4;

    // frill glow
    float frillCore = (1.0 - smoothstep (0.0, halfW, abs (fu))) * step (r0 - 0.02, fr) * step (fr, r0 + flen + 0.01);
    frillCore *= step (fang, 1.75);
    float frillGlow = exp (-max (dFrill, 0.0) * 55.0);
    // (dimmed while asleep: the vocoder may still be on, but a sleeping creature's ridges go dark)
    col += uColVocoder * uGravelOn * (frillCore * (0.25 + 1.4 * band) * (1.0 - along * 0.6) + frillGlow * band * 0.35)
         * (0.3 + 0.7 * awake) * (0.25 + 0.75 * Fh) * (1.0 - gl);

    // eyes (softened: they bloom through the cloud instead of sitting on it)
    float keep = 1.0 - gl;
    float eye = (1.0 - smoothstep (-aaE, aaE, dEye)) * Fe * keep;
    vec3 eyeCol = mix (uColOutput, uColRoar, clamp (heat * 1.3, 0.0, 1.0));
    float eI = ((0.75 + 1.7 * heat + 0.5 * uLevel) * awake + 0.12 * uSleep) * mix (0.75, 1.0, min (glowK, 1.0)) * (0.8 + 0.2 * glowK);
    float iris = exp (-length (e / vec2 (ew * 0.8, 0.032)) * 2.0);
    float slit = 1.0 - smoothstep (0.003, 0.007 + 0.004 * (1.0 - heat) + 0.004 * sigma, abs (h.x - ec.x));
    vec3 eyeFill = eyeCol * (0.35 + 1.2 * iris) * eI;
    eyeFill = mix (eyeFill, vec3 (0.02, 0.0, 0.0), slit * eyeOpen * (0.55 + 0.3 * (1.0 - sigma)));
    eyeFill += vec3 (1.0, 0.95, 0.8) * pow (iris, 6.0) * 0.5 * eI;
    col = mix (col, eyeFill, eye);
    float eyeHalo = exp (-max (dEye, 0.0) / 0.04) * (1.0 - eye) * Fe * keep;
    col += eyeCol * exp (-max (dEye, 0.0) * 32.0) * eI * 0.18 * (1.0 - eye) * Fe * keep;
    float lidLine = exp (-abs (e.y + 0.006 * (1.0 - (e.x * e.x) / (ew * ew))) / 0.0025) * (1.0 - smoothstep (ew * 0.8, ew, abs (e.x)));
    col += eyeCol * lidLine * 0.45 * uSleep * Fe * keep;

    // the extra eyes (dimmer, in the same light)
    vec3 bright = smoothstep (0.15, 0.4, openX) * Fe * keep;
    float exHalo = 0.0;
    if (openX.x + openX.y + openX.z > 0.001)
    {
        vec3 exCol = mix (eyeCol, uColWashB, 0.25 * uEvoPal.w);
        float exI = eI * 0.85;
        col = mix (col, exCol * (0.3 + 1.1 * eyB.z) * exI, eyB.x * bright.x);
        col = mix (col, exCol * (0.3 + 1.1 * eyK.z) * exI, eyK.x * bright.y);
        col = mix (col, exCol * (0.3 + 1.1 * eyC.z) * exI * 1.1, eyC.x * bright.z);
        exHalo = eyB.y * bright.x + eyK.y * bright.y + eyC.y * bright.z;
        // a slit of glow before the lids open
        col += exCol * exI * 0.25 * (eyB.y * eyB.y * bright.x * (1.0 - eyB.x) + eyK.y * eyK.y * bright.y * (1.0 - eyK.x)
                                     + eyC.y * eyC.y * bright.z * (1.0 - eyC.x)) * 0.5;
    }

    // maw: a hot throat at the back fading to the cloud's colour at the lips
    // (soft while the face is still cloud, sharper when loud; the edge is fogged further down)
    float aaM = aa + 0.012 * sigma;
    float mouthShape = 1.0 - smoothstep (-aaM, aaM, dMouth);
    float mouth = mouthShape * Fm * keep;
    float subPulse = uSub * uSubLevel * (0.5 + 0.5 * sin (uSubPhase * 6.2832 * 3.0));
    float tg = exp (-length ((h - vec2 (0.0, mouthMid)) / vec2 (mw * 0.75, open * 0.6 + 0.015)) * 1.5);
    float meanBand = 0.0;
    for (int i = 0; i < 32; i += 4)
        meanBand += uBands[i] * 0.125;
    vec3 throatCol = mix (vec3 (0.20, 0.015, 0.03), uColRoar, 0.35 + 0.65 * heat);
    throatCol = mix (throatCol, uColVocoder, uGravelOn * clamp (meanBand * 1.5, 0.0, 0.7));
    float throatI = (0.35 + 1.3 * heat + 0.6 * uLevel + 0.8 * subPulse) * awake * mix (0.8, 1.0, min (glowK, 1.0));
    vec3 throatFloor = mix (vec3 (0.05, 0.004, 0.01), uColCloud * 0.25, 0.6);
    // soft-clipped, so even a full roar keeps a falloff from the back of the throat to the lips
    vec3 throat = throatFloor + throatCol * 1.4 * (1.0 - exp (-tg * throatI / 1.4));
    float lipShade = smoothstep (0.0, 0.01 + 0.012 * sigma, min (yU - h.y, h.y - yL));
    throat = mix (mix (bodyCol, throatFloor, 0.5), throat, 0.3 + 0.7 * lipShade);
    throat += uColGrowl * (1.0 - lipShade) * uGrowl * abs (uGrowlMod) * 0.6 * awake;
    throat *= 1.0 - 0.6 * uOvReplace * ovA;   // Replace: the throat goes hollow
    col = mix (col, throat, mouth);
    // before the mouth has a shape, its glow is already there in the cloud
    col += throatCol * exp (-max (dMouth, 0.0) / 0.03) * throatI * 0.35 * Fe * (1.0 - Fm) * keep;

    // teeth: two rows behind, the main rows on the lips (they interlock when the jaw is shut);
    // translucent at the tips, a little of the cloud's colour in the bone
    float tremor = uGrowl * awake * (0.0025 * sin (t * 83.0) + 0.002 * uGrowlMod) * (0.3 + uJaw);
    float tw = mw / 5.0;
    float teethZone = 1.0 - smoothstep (mw * 0.92, mw * 1.0, h.x);
    float baseLen = (0.024 + 0.022 * size) * uEvoMaw.y;
    vec3 bone = mix (vec3 (0.80, 0.75, 0.66), uColText, 0.3);
    bone = mix (bone, mix (uColCloud, uColCloudLit, 0.7), 0.2 + 0.15 * sigma);
    vec3 glowOnBone = throatCol * tg * (0.4 + heat) * awake;
    vec3 boneCloud = mix (uColCloud, uColCloudLit, 0.55);
    float softT = 1.0 + 3.0 * sigma;
    float teethF = Fm * keep;
    float deessGlint = uDeess * (0.6 + 0.4 * sin (t * 3.0 + h.x * 40.0)) * awake;

    float u3 = h.x / (tw * 0.72) + 0.25;
    float f3 = fract (u3) - 0.5;
    float tc3 = 1.0 - pow (abs (f3) * 2.0, 1.3);
    float back = max ((yU - 0.006 - baseLen * 0.55 * tc3) - h.y, h.y - yU);
    back = min (back, max (h.y - (yL + 0.004 + baseLen * 0.45 * tc3), yL - h.y));
    float backTeeth = (1.0 - smoothstep (-fwidth (back) * softT, fwidth (back) * softT, back)) * mouthShape * teethZone;
    col = mix (col, bone * 0.22 + glowOnBone * 0.3, backTeeth * teethF * Fm * 0.6);
    col += uColDeess * backTeeth * deessGlint * 0.3 * teethF;

    // a third row of thin needles on the liquid and stranger forms
    if (uEvoMaw.z > 0.001)
    {
        float u4 = h.x / (tw * 0.45) + 0.5;
        float f4 = fract (u4) - 0.5;
        float tc4 = 1.0 - pow (abs (f4) * 2.0, 0.6);
        float L4 = baseLen * 1.15 * tc4 * uEvoMaw.z;
        float dN = max ((yU - 0.003 - L4) - h.y, h.y - (yU - 0.002));
        dN = min (dN, max (h.y - (yL + 0.003 + L4 * 0.8), (yL + 0.002) - h.y));
        float needles = (1.0 - smoothstep (-fwidth (dN) * softT, fwidth (dN) * softT, dN)) * mouthShape * teethZone;
        col = mix (col, bone * 0.55 + glowOnBone * 0.5, needles * teethF * 0.85);
        col += uColDeess * needles * deessGlint * 0.4 * teethF;
    }

    float u2 = h.x / tw + 0.5;
    float k2 = floor (u2);
    float f2 = fract (u2) - 0.5;
    // each tooth its own for the seed: some long, some broken off, a few missing
    vec3 tv2 = toothVar (k2, 2.0);
    float tc2 = 1.0 - pow (min (abs (f2) * 2.0 / tv2.z, 1.0), 1.25);
    tc2 = min (tc2, tv2.y);
    float fang2 = (1.05 - 0.07 * k2) * tv2.x;
    float L2 = baseLen * 0.8 * fang2;
    float yLt = yL - tremor;
    float dLow = max (h.y - (yLt + L2 * tc2), (yLt - 0.008) - h.y);
    float lowTeeth = (1.0 - smoothstep (-fwidth (dLow) * softT, fwidth (dLow) * softT, dLow)) * teethZone;
    float lowShade = clamp ((h.y - yLt) / max (L2, 1.0e-3), 0.0, 1.0);
    vec3 lowCol = bone * (0.45 + 0.55 * lowShade) * (0.75 + 0.25 * tc2) + glowOnBone * 0.5;
    lowCol = mix (lowCol, boneCloud, 0.45 * (1.0 - lowShade) * (0.4 + 0.6 * sigma));   // rooted in the cloud
    float lowA = lowTeeth * smoothstep (0.0, 0.02, -dInk + 0.02) * mix (1.0, 0.55, lowShade * sigma) * teethF;
    col = mix (col, lowCol, lowA);
    col += uColDeess * lowA * pow (tc2, 3.0) * deessGlint * 0.5;

    float u1 = h.x / tw;
    float k1 = floor (u1);
    float f1 = fract (u1) - 0.5;
    vec3 tv1 = toothVar (k1, 1.0);
    float tc1 = 1.0 - pow (min (abs (f1) * 2.0 / tv1.z, 1.0), 1.25);
    tc1 = min (tc1, tv1.y);
    float fang1 = (1.1 - 0.08 * k1) * tv1.x;
    float L1 = baseLen * fang1;
    float yUt = yU + tremor;
    float dUp = max ((yUt - L1 * tc1) - h.y, h.y - (yUt + 0.008));
    float upTeeth = (1.0 - smoothstep (-fwidth (dUp) * softT, fwidth (dUp) * softT, dUp)) * teethZone;
    float upShade = clamp ((yUt - h.y) / max (L1, 1.0e-3), 0.0, 1.0);
    vec3 upCol = bone * (0.5 + 0.5 * upShade) * (0.75 + 0.25 * tc1) + glowOnBone * 0.35;
    upCol += vec3 (1.0) * pow (tc1, 8.0) * upShade * 0.12;
    upCol = mix (upCol, boneCloud, 0.45 * (1.0 - upShade) * (0.4 + 0.6 * sigma));
    float upA = upTeeth * smoothstep (0.0, 0.02, -dInk + 0.02) * mix (1.0, 0.55, upShade * sigma) * teethF;
    col = mix (col, upCol, upA);
    // de-ess: a cold glint runs along the teeth on each S
    col += uColDeess * upA * pow (tc1, 3.0) * (0.4 + 0.6 * upShade) * deessGlint;

    // ring: metal plates over the mouth, flickering at a slowed copy of the carrier
    if (uRing > 0.001)
    {
        float top = mY + 0.032 + 0.22 * open;
        float bot = yL - 0.016 - 0.01 * uRingBody;
        float pw = mw * (1.12 + 0.08 * uRingBody);
        float edge = min (pw - h.x, min (top - h.y, h.y - bot));
        float plate = smoothstep (0.0, 0.003, edge);
        float slats = 3.0 + 2.0 * uRingBody;
        float sv = fract ((top - h.y) / max (top - bot, 0.01) * slats);
        float slat = smoothstep (0.04, 0.16, sv) * smoothstep (0.96, 0.84, sv);
        float flk = 0.55 + 0.45 * uRingFlicker;
        vec3 metal = mix (vec3 (0.10, 0.11, 0.13), uColRing, 0.25 + 0.6 * (1.0 - sv));
        metal += vec3 (1.0) * pow (1.0 - abs (sv - 0.3) * 2.0, 8.0) * 0.35;          // a bright line on each slat
        metal *= 0.45 + 0.55 * flk;
        float rivet = 0.0;
        for (int i = 0; i < 3; i++)
        {
            float rx = pw * (0.2 + 0.35 * float (i));
            rivet += (1.0 - smoothstep (0.0035, 0.0055, length (h - vec2 (rx, top - 0.007)))) * uRingBody;
        }
        vec3 gap = uColRing * 0.35 * uRingFlicker + vec3 (0.02);
        vec3 plateCol = mix (gap, metal, slat) + vec3 (0.9) * rivet;
        float plateA = clamp (0.3 + 1.4 * uRing, 0.0, 1.0) * smoothstep (0.0, 0.03, uRing) * Fm * keep;
        col = mix (col, plateCol, plate * plateA);
        col += uColRing * exp (-max (-edge, 0.0) * 120.0) * (1.0 - plate) * uRing * 0.25 * flk * Fm * keep;
    }

    // rings and embers also play faintly over the creature
    col += uColSub * rings * subAmt * 0.18 * ink;
    col += uColRoar * embers * heat * 0.8 * awake * ink;

    // bioluminescent dots along the wing edges and the spines
    if (uEvoPal.w > 0.001)
    {
        vec2 bc = wq * 26.0;
        vec2 bcell = floor (bc);
        float bh = hash12 (bcell + 31.7);
        vec2 bo = vec2 (hash12 (bcell + 5.3), hash12 (bcell + 9.1)) * 0.6 + 0.2;
        float bdd = length (fract (bc) - bo);
        float along2 = exp (-abs (dEdge + 0.012) / 0.02) + smoothstep (0.02, -0.005, dCrown) + smoothstep (0.015, -0.005, dSpur);
        float dot1 = step (0.62, bh) * (1.0 - smoothstep (0.08, 0.22, bdd)) * along2;
        float pulse = 0.55 + 0.45 * sin (t * (0.8 + 1.7 * bh) + bh * 40.0);
        vec3 bcol = mix (uColWashA, uColWashB, step (0.5, fract (bh * 7.0)));
        bcol = mix (bcol, vec3 (0.55, 0.95, 1.0), 0.35);
        col += bcol * dot1 * pulse * uEvoPal.w * 0.9 * Fx * keep * (0.4 + 0.6 * awake);
    }

    // the output clipping: sparks and crackle along the edge of the ink (and out in the wisps)
    if (uClip > 0.001)
    {
        float cell = hash12 (floor (frag / (3.0 * uPxScale)) + floor (t * 24.0) * 7.31);
        float edgeBand = exp (-abs (dEdge) * 90.0) + wisp * 0.5;
        float spark = step (0.993 - 0.025 * uClip, cell) * edgeBand
                    * (1.0 - smoothstep (0.15, 0.5, length (fract (frag / (3.0 * uPxScale)) - 0.5)));
        float crackle = (1.0 - smoothstep (0.0, 0.04, abs (vnoise (frag * 0.08 / uPxScale + vec2 (t * 13.0, 0.0)) - 0.5))) * edgeBand;
        col += (vec3 (1.0, 0.88, 0.65) * spark * 1.6 + uColRoar * crackle * 0.45) * uClip * awake;
    }

    // ---- front fog: drifting in front of the creature (not mirrored); denser over the lower face
    // and at the silhouette, thinner over the eyes; sub rings push it outward
    vec2 radial = (p - mc) / max (length (p - mc), 1.0e-3);
    float Sfr = Sf;
    if (subAmt > 0.001)
        Sfr = streak (cwF + 0.6 * warp - radial * rings * subAmt * 0.026, flowAng * 0.8 + 0.12);
    float frontLayer = (Sfr + 0.3 * C) / 1.3;
    float lowerFog = smoothstep (mY, mY - 0.25, h.y) * (1.0 - smoothstep (0.25, 0.6, h.x)) * (1.0 - gl);
    frontLayer += 0.6 * lowerFog + 0.4 * exp (-abs (dEdge) / 0.05) * Fh * (1.0 - 0.6 * gl);
    frontLayer += 0.35 * exp (-abs (dMouth) / 0.015) * Fm * keep;   // the lips sit in the fog too
    vec2 eRel = (h - ec) / 0.085;
    float eyeGlow = clamp (eI * 0.5, 0.0, 1.0) * Fe * keep;
    float thinEyes = mix (0.12, 0.45, eyeGlow) * exp (-dot (eRel, eRel)) * smoothstep (0.0, 0.2, eyeGlow);
    float fogFront = fogA * smoothstep (0.25, 0.9, frontLayer) * (1.0 - thinEyes);
    fogFront += 0.15 * uGateClosed * awake * lowerFog;
    fogFront = clamp (fogFront, 0.0, 0.6);
    vec3 fogCol = mix (uColCloud, uColCloudLit, 0.40 + 0.35 * Sfr) + uColUnderglow * heat * awake * 0.12;
    col = mix (col, fogCol, fogFront);

    // the eyes' light scatters in the fog: the halo grows where the fog is dense
    float bloomK = 1.0 + 1.5 * fogFront;
    col += eyeCol * eyeHalo * eI * 0.20 * bloomK;
    col += mix (eyeCol, uColWashB, 0.25 * uEvoPal.w) * exHalo * eI * 0.10 * bloomK;
    col += throatCol * tg * mouth * throatI * fogFront * 0.35;

    col = col / (1.0 + col * 0.22);
    col = clamp (col, 0.0, 1.0);

    // rounded corners, filled with what is around the view
    vec2 cq2 = abs (frag - 0.5 * uRes) - (0.5 * uRes - vec2 (uRadius));
    float dc = length (max (cq2, 0.0)) - uRadius;
    col = mix (col, uColOuter, smoothstep (-0.75, 0.75, dc));

    gl_FragColor = vec4 (col, 1.0);
}
)GLSL";

    const char* const c_uniformNames[] =
    {
        "uRes", "uPxScale", "uRadius", "uTime", "uJaw", "uGrowl", "uGrowlMod", "uLiquid", "uLiquidPhase", "uRoar",
        "uRoarDrive", "uSub", "uSubLevel", "uSubPhase", "uPitch", "uSize", "uGravelOn", "uDetailLow", "uDetailHigh",
        "uBoost", "uCut", "uSleep", "uBlink", "uLevel", "uBands",
        "uSwirlNotch", "uSwirlAmt", "uEnsWidth", "uDeess", "uTame", "uGateClosed", "uOvLevel", "uOvHarm", "uOvReplace",
        "uRing", "uRingFlicker", "uRingBody", "uStrain", "uStrainPulse", "uClip",
        "uColScope", "uColGrid", "uColOuter", "uColPitch", "uColGrowl", "uColLiquid", "uColRoar", "uColSub",
        "uColDetail", "uColVocoder", "uColOutput", "uColText", "uColSwirl", "uColOvertone", "uColRing", "uColDeess", "uColEnsemble",
        "uEvoHead", "uEvoWing", "uEvoHorn", "uEvoEyes", "uEvoMaw", "uEvoInk", "uEvoFlow", "uEvoPal", "uEvoMisc", "uForm", "uCloud", "uSeed", "uSeedB",
        "uDrift", "uFlow", "uFlow2", "uColCloud", "uColCloudLit", "uColUnderglow", "uColRedInk", "uColWashA", "uColWashB", "uEgg", "uLogoBox", "uLogoSdf"
    };

    // Cloud advection, in cloud-field units per second (the back layer; the front one is 1.6x).
    // About a hundredth of the view's height per second, a little downward (heavier with ink).
    constexpr double c_windX = 0.013, c_windY = -0.004, c_windInkY = -0.006;
    constexpr double c_driftWrap = 4096.0;     // offsets wrap (days apart at these rates) to keep float precision
    constexpr double c_flowRadius = 64.0;      // the slow scrolls' circles in noise space (an hour or more round)
    constexpr double c_bloomSeconds = 70.0;    // the bloom threshold's slow drift
    constexpr float c_logoBoxSize = 0.8f;      // of the view height, centred
}

MonsterView::MonsterView (const roar::MonsterFeatures& features, const roar::CreatureShared* creature)
    : m_features (features),
      m_ownCreature (creature == nullptr ? std::make_unique<roar::CreatureShared>() : nullptr),
      m_creature (creature != nullptr ? *creature : *m_ownCreature)
{
    static_assert (std::size (c_uniformNames) == numUniforms, "one name per uniform");

    // Not opaque: the overlay (readouts, fallback text) is painted over the GL frame.
    setOpaque (false);
    // Clicks are taken only for the right-click menu; nothing else happens on a click.
    setInterceptsMouseClicks (true, false);

    m_context.setOpenGLVersionRequired (juce::OpenGLContext::openGL3_2);
    m_context.setRenderer (this);
    m_context.setComponentPaintingEnabled (true);
    // No continuous repainting: the shader is a full-screen procedural one, so drawing it at the
    // display rate (up to 120 Hz) costs a lot of GPU for no visible gain. The timer below asks for
    // frames at 30 Hz, and at 15 Hz once the creature is asleep and settled.
    m_context.setContinuousRepainting (false);
    m_context.attachTo (*this);

    // The timer also refreshes the overlay text, which only repaints when it changes.
    startTimerHz (c_frameHz);
}

MonsterView::~MonsterView()
{
    stopTimer();
    m_context.detach();
    m_creature.eggVisible.store (false, std::memory_order_relaxed);
}

void MonsterView::setParams (const Params& params)
{
    m_pitchSt.store (params.pitchSt, std::memory_order_relaxed);
    m_size.store (params.size, std::memory_order_relaxed);
    m_growl.store (params.growl, std::memory_order_relaxed);
    m_liquid.store (params.liquid, std::memory_order_relaxed);
    m_roar.store (params.roar, std::memory_order_relaxed);
    m_sub.store (params.sub, std::memory_order_relaxed);
    m_gravel.store (params.gravel, std::memory_order_relaxed);
    m_detailLow.store (params.detailLow, std::memory_order_relaxed);
    m_detailHigh.store (params.detailHigh, std::memory_order_relaxed);
    m_swirlKnob.store (params.swirl, std::memory_order_relaxed);
    m_ensembleKnob.store (params.ensemble, std::memory_order_relaxed);
    m_overtoneKnob.store (params.overtone, std::memory_order_relaxed);
    m_ringKnob.store (params.ring, std::memory_order_relaxed);
    m_cleanOn.store (params.cleanOn, std::memory_order_relaxed);
}

void MonsterView::mouseDown (const juce::MouseEvent& event)
{
    if (event.mods.isPopupMenu())
        showCreatureMenu();
}

void MonsterView::showCreatureMenu()
{
    if (! isCreatureFrozen || ! setCreatureFrozen || ! resetCreature)
        return;

    enum { live = 1, frozen, reset };
    const bool isFrozen = isCreatureFrozen();
    juce::PopupMenu menu;
    menu.addSectionHeader ("Creature");
    menu.addItem (live, "Evolution: Live", true, ! isFrozen);
    menu.addItem (frozen, "Evolution: Frozen", true, isFrozen);
    menu.addSeparator();
    menu.addItem (reset, "Reset creature");
    menu.showMenuAsync (RoarschachLookAndFeel::menuOptionsFor (*this),
                        [safe = juce::Component::SafePointer<MonsterView> (this)] (int item)
                        {
                            if (safe == nullptr || item == 0)
                                return;
                            if (item == live || item == frozen)
                                safe->setCreatureFrozen (item == frozen);
                            else if (item == reset)
                                safe->resetCreature();
                        });
}

juce::Image MonsterView::takeCapture()
{
    const juce::ScopedLock lock (m_captureLock);
    auto image = m_capture;
    m_capture = {};
    return image;
}

void MonsterView::setCorner (float radius, juce::Colour outside)
{
    m_cornerRadius.store (std::max (0.0f, radius), std::memory_order_relaxed);
    m_outsideArgb.store (outside.getARGB(), std::memory_order_relaxed);
}

void MonsterView::resized()
{
    m_width.store (getWidth(), std::memory_order_relaxed);
    m_height.store (getHeight(), std::memory_order_relaxed);
}

void MonsterView::timerCallback()
{
    // GL frames: full rate while awake, a slow tick once asleep and settled.
    m_frameTick = (m_frameTick + 1) % c_idleFrameDivider;

    if (! m_idle.load (std::memory_order_relaxed) || m_frameTick == 0)
        m_context.triggerRepaint();

    const float f0 = m_features.f0Hz.load (std::memory_order_relaxed);
    const bool voiced = m_features.voiced.load (std::memory_order_relaxed) > 0.5f;
    const float boost = m_features.detailBoostDb.load (std::memory_order_relaxed);
    const float cut = m_features.detailCutDb.load (std::memory_order_relaxed);

    const bool stale = m_stale.load (std::memory_order_relaxed);
    const bool live = ! stale && voiced && std::isfinite (f0) && f0 > 0.0f && f0 < 20000.0f;
    const auto f0Text = live ? "F0 " + juce::String (juce::roundToInt (f0)) + " Hz" : juce::String ("F0 --");
    // Same rule as the strip's DETAIL readout: no sign on a zero, so it never reads "-0.0".
    const auto dbText = [stale] (float db, const char* sign)
    {
        const float v = ! stale && std::isfinite (db) ? std::min (std::abs (db), 99.9f) : 0.0f;
        return v < 0.05f ? juce::String ("0.0") : sign + juce::String (v, 1);
    };
    const auto detailText = "DETAIL " + dbText (boost, "+") + " / " + dbText (cut, "-") + " dB";
    const bool gravel = ! stale && m_features.gravelActive.load (std::memory_order_relaxed) > 0.5f;
    const bool asleep = m_asleep.load (std::memory_order_relaxed);

    // attachTo() succeeds even when the native context can't be made, and then no GL callback ever
    // runs: give it a few seconds on screen before showing the fallback.
    const bool glReady = m_glReady.load (std::memory_order_relaxed);
    m_ticksWithoutGL = (glReady || ! isShowing()) ? 0 : std::min (m_ticksWithoutGL + 1, c_failAfterTicks);
    const bool failed = m_shaderFailed.load (std::memory_order_relaxed) || ! m_context.isAttached()
                     || m_ticksWithoutGL >= c_failAfterTicks;

    if (f0Text != m_f0Text || detailText != m_detailText || gravel != m_gravelShown
        || asleep != m_sleepShown || failed != m_failShown)
    {
        m_f0Text = f0Text;
        m_detailText = detailText;
        m_gravelShown = gravel;
        m_sleepShown = asleep;
        m_failShown = failed;
        repaint();
    }
}

void MonsterView::paint (juce::Graphics& g)
{
    using namespace Theme::Colours;

    const auto bounds = getLocalBounds().toFloat();
    const float radius = m_cornerRadius.load (std::memory_order_relaxed);

    if (m_failShown)
    {
        g.setColour (scope);
        g.fillRoundedRectangle (bounds, radius);
        g.setColour (textDim);
        g.setFont (m_fonts->body (12.0f));
        g.drawText ("OpenGL not available", bounds, juce::Justification::centred, false);
    }

    auto row = bounds.reduced (16.0f, 12.0f).removeFromTop (16.0f);
    g.setFont (m_fonts->label (10.5f));

    g.setColour (m_f0Text == "F0 --" ? textFaint : pitch.withAlpha (0.9f));
    g.drawText (m_f0Text, row, juce::Justification::centredLeft, false);

    g.setColour (Theme::Colours::detail.withAlpha (0.85f));
    g.drawText (m_detailText, row, juce::Justification::centredRight, false);

    auto bottom = bounds.reduced (16.0f, 12.0f).removeFromBottom (16.0f);

    if (m_gravelShown)
    {
        g.setColour (vocoder.withAlpha (0.85f));
        g.drawText ("VOCODER  32 BANDS", bottom, juce::Justification::centredLeft, false);
    }

    if (m_sleepShown)
    {
        g.setColour (textFaint);
        g.drawText ("ASLEEP", bottom, juce::Justification::centredRight, false);
    }

    g.setColour (outlineSoft);
    g.drawRoundedRectangle (bounds.reduced (0.5f), radius, 1.0f);
}

bool MonsterView::buildShader()
{
    auto program = std::make_unique<juce::OpenGLShaderProgram> (m_context);

    if (! program->addVertexShader (juce::OpenGLHelpers::translateVertexShaderToV3 (c_vertexShader))
        || ! program->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (c_fragmentShader))
        || ! program->link())
    {
        DBG ("MonsterView shader: " << program->getLastError());
        return false;
    }

    using namespace juce::gl;
    const auto id = program->getProgramID();

    for (int i = 0; i < numUniforms; ++i)
        m_uniforms[(size_t) i] = glGetUniformLocation (id, c_uniformNames[i]);

    m_positionAttrib = glGetAttribLocation (id, "aPosition");

    if (m_positionAttrib < 0)
        return false;

    m_program = std::move (program);
    return true;
}

void MonsterView::newOpenGLContextCreated()
{
    using namespace juce::gl;

    m_firstFrame = true;

    if (! buildShader())
    {
        m_shaderFailed.store (true, std::memory_order_relaxed);
        return;
    }

    // Two triangles covering the viewport.
    static constexpr GLfloat quad[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };

    glGenBuffers (1, &m_vbo);
    glBindBuffer (GL_ARRAY_BUFFER, m_vbo);
    glBufferData (GL_ARRAY_BUFFER, sizeof (quad), quad, GL_STATIC_DRAW);

    // Core profile: the attribute layout lives in our own VAO. Legacy: it is set per draw and
    // disabled again, so JUCE's component painting never sees our attribute array left enabled.
    if (m_context.isCoreProfile())
    {
        glGenVertexArrays (1, &m_vao);
        glBindVertexArray (m_vao);
        glEnableVertexAttribArray ((GLuint) m_positionAttrib);
        glVertexAttribPointer ((GLuint) m_positionAttrib, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray (0);
    }

    glBindBuffer (GL_ARRAY_BUFFER, 0);

    // The logo's distance field, for the hidden morph (built once per process on the message thread).
    if (! m_logo.rgba.empty())
    {
        glGenTextures (1, &m_logoTexture);
        glBindTexture (GL_TEXTURE_2D, m_logoTexture);
        glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, LogoSdf::c_size, LogoSdf::c_size, 0, GL_RGBA, GL_UNSIGNED_BYTE, m_logo.rgba.data());
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture (GL_TEXTURE_2D, 0);
    }

    // The egg can be seen only while this context has the logo to draw.
    m_creature.eggVisible.store (m_logoTexture != 0, std::memory_order_relaxed);

    m_evoSnap = true;
    m_shaderFailed.store (false, std::memory_order_relaxed);
    m_glReady.store (true, std::memory_order_relaxed);
}

void MonsterView::openGLContextClosing()
{
    m_creature.eggVisible.store (false, std::memory_order_relaxed);
    m_glReady.store (false, std::memory_order_relaxed);
    releaseGL();
}

void MonsterView::releaseGL()
{
    using namespace juce::gl;

    m_program.reset();

    if (m_vbo != 0)
        glDeleteBuffers (1, &m_vbo);

    if (m_vao != 0)
        glDeleteVertexArrays (1, &m_vao);

    if (m_logoTexture != 0)
        glDeleteTextures (1, &m_logoTexture);

    m_vbo = 0;
    m_vao = 0;
    m_logoTexture = 0;
    m_positionAttrib = -1;
}

void MonsterView::updateAnimation (float dt)
{
    const auto& f = m_features;

    const int generation = f.generation.load (std::memory_order_relaxed);

    if (generation != m_lastGeneration)
    {
        m_lastGeneration = generation;
        m_staleSeconds = 0.0f;
    }
    else
    {
        m_staleSeconds = std::min (m_staleSeconds + dt, 60.0f);
    }

    // Stale features read as silence: the creature closes its mouth and drifts off to sleep.
    const bool stale = m_staleSeconds >= c_staleAfterSeconds;
    m_stale.store (stale, std::memory_order_relaxed);
    const float live = stale ? 0.0f : 1.0f;
    const auto load = [live] (const std::atomic<float>& a) { return a.load (std::memory_order_relaxed) * live; };

    const float input = safe (load (f.inputLevel), 0.0f, 4.0f);

    // Asleep after 1.5 s of silence; wakes quickly when sound comes back.
    m_quietSeconds = input < c_quietLevel ? std::min (m_quietSeconds + dt, 60.0f) : 0.0f;
    const bool asleep = m_quietSeconds >= c_sleepAfterSeconds;
    follow (m_sleep, asleep ? 1.0f : 0.0f, dt, 0.9f, 0.15f);
    m_asleep.store (asleep, std::memory_order_relaxed);

    follow (m_jaw, safe (load (f.jaw), 0.0f, 1.0f), dt, 0.012f, 0.060f);
    follow (m_growlMod, safe (load (f.growlMod), -1.0f, 1.0f), dt, 0.008f, 0.008f);
    follow (m_roarDrive, safe (load (f.roarHeat), 0.0f, 1.0f), dt, 0.060f, 0.25f);
    follow (m_subLevel, safe (load (f.subLevel), 0.0f, 1.0f), dt, 0.030f, 0.20f);
    follow (m_level, safe (load (f.outputLevel), 0.0f, 1.0f), dt, 0.030f, 0.20f);
    // Either sign convention for the cut works: only the amount is drawn.
    follow (m_boost, std::abs (safe (load (f.detailBoostDb), -c_detailBoostRangeDb, c_detailBoostRangeDb)) / c_detailBoostRangeDb, dt, 0.08f, 0.25f);
    follow (m_cut, std::abs (safe (load (f.detailCutDb), -c_detailCutRangeDb, c_detailCutRangeDb)) / c_detailCutRangeDb, dt, 0.08f, 0.25f);
    follow (m_gravelOn, load (f.gravelActive) > 0.5f ? 1.0f : 0.0f, dt, 0.15f, 0.30f);

    for (size_t i = 0; i < m_bands.size(); ++i)
        follow (m_bands[i], safe (load (f.vocoderBands[i]), 0.0f, 1.0f), dt, 0.015f, 0.12f);

    const auto knob = [dt] (float& state, const std::atomic<float>& target, float lo, float hi)
    {
        follow (state, safe (target.load (std::memory_order_relaxed), lo, hi), dt, 0.12f, 0.12f);
    };
    knob (m_knobs.pitchSt, m_pitchSt, -24.0f, 12.0f);
    knob (m_knobs.size, m_size, 0.0f, 1.0f);
    knob (m_knobs.growl, m_growl, 0.0f, 1.0f);
    knob (m_knobs.liquid, m_liquid, 0.0f, 1.0f);
    knob (m_knobs.roar, m_roar, 0.0f, 1.0f);
    knob (m_knobs.sub, m_sub, 0.0f, 1.0f);
    knob (m_knobs.gravel, m_gravel, 0.0f, 1.0f);
    knob (m_knobs.detailLow, m_detailLow, 0.0f, 1.0f);
    knob (m_knobs.detailHigh, m_detailHigh, 0.0f, 1.0f);
    knob (m_knobs.swirl, m_swirlKnob, 0.0f, 1.0f);
    knob (m_knobs.ensemble, m_ensembleKnob, 0.0f, 1.0f);
    knob (m_knobs.overtone, m_overtoneKnob, 0.0f, 1.0f);
    knob (m_knobs.ring, m_ringKnob, 0.0f, 1.0f);

    // The other modules (each feature already fades to 0 with its module's On switch).
    follow (m_swirlNotch, safe (load (f.swirlNotch), 0.0f, 1.0f), dt, 0.05f, 0.05f);
    follow (m_ensWidth, safe (load (f.ensembleWidth), 0.0f, 1.0f), dt, 0.15f, 0.30f);
    follow (m_deess, std::abs (safe (load (f.deessGrDb), -24.0f, 24.0f)) / 8.0f, dt, 0.01f, 0.12f);
    follow (m_tame, std::abs (safe (load (f.tameGrDb), -24.0f, 24.0f)) / 6.0f, dt, 0.05f, 0.30f);
    const float gate = safe (load (f.gateOpen), 0.0f, 1.0f);
    const float cleanOn = safe (m_cleanOn.load (std::memory_order_relaxed), 0.0f, 1.0f);
    follow (m_gate, 1.0f - cleanOn * (1.0f - gate), dt, 0.02f, 0.25f);
    follow (m_ovLevel, safe (load (f.overtoneLevel), 0.0f, 1.0f), dt, 0.03f, 0.20f);
    for (size_t k = 0; k < m_ovHarm.size(); ++k)
        follow (m_ovHarm[k], safe (load (f.overtoneHarm[k]), 0.0f, 1.0f), dt, 0.02f, 0.15f);
    follow (m_ovReplace, safe (load (f.overtoneReplace), 0.0f, 1.0f), dt, 0.2f, 0.2f);
    follow (m_ring, safe (load (f.ringAmount), 0.0f, 1.0f), dt, 0.1f, 0.2f);
    follow (m_ringBody, safe (load (f.ringBodyMix), 0.0f, 1.0f), dt, 0.1f, 0.2f);
    {
        // The plates flicker at the carrier, halved until it can be seen (12 Hz or slower).
        float rate = safe (load (f.ringHz), 0.0f, 20000.0f);
        while (rate > 12.0f)
            rate *= 0.5f;
        m_ringPhase = std::fmod (m_ringPhase + dt * rate, 1.0f);
    }
    follow (m_strain, std::abs (safe (load (f.limiterGrDb), -48.0f, 48.0f)) / 9.0f, dt, 0.03f, 0.35f);
    m_strainPhase = std::fmod (m_strainPhase + dt * (1.1f + 1.6f * std::min (m_strain, 1.0f)), 1.0f);
    follow (m_clip, std::abs (safe (load (f.clipDb), -48.0f, 48.0f)) / 4.0f, dt, 0.01f, 0.25f);

    // Rings travel faster while the sub is loud.
    m_subPhase = std::fmod (m_subPhase + dt * (0.30f + 0.9f * m_subLevel), 1.0f);

    // The evolved form: the processor publishes it ten times a second; smooth it so it never steps.
    {
        using E = roar::CreatureEvolution;
        const auto target = m_creature.read();
        const float k = m_evoSnap ? 1.0f : smoothing (dt, 0.15f);
        // The blot offsets are positions in noise space: they are not smoothed. When they move (a
        // reset, or a queued seed taking over) the glide between them is taken as published too:
        // the published glide already moves smoothly, and an old glide value with new endpoints
        // would show the new blot for a moment and swing back.
        const auto moved = [&target, this] (int i) { return std::abs (target[(size_t) i] - m_evo[(size_t) i]) > 1.0e-6f; };
        const bool endpointsMoved = moved (E::seedAX) || moved (E::seedAY) || moved (E::seedBX) || moved (E::seedBY);
        for (size_t i = 0; i < m_evo.size(); ++i)
        {
            const bool offset = i == (size_t) E::seedAX || i == (size_t) E::seedAY || i == (size_t) E::seedBX || i == (size_t) E::seedBY;
            const bool snap = offset || (endpointsMoved && i == (size_t) E::seedGlide);
            m_evo[i] = snap ? target[i] : m_evo[i] + (target[i] - m_evo[i]) * k;
        }

        // Formation: a roar pulls the face out of the cloud now, sleep disperses it again.
        const float loud = juce::jmap (juce::jlimit (-36.0f, -10.0f, juce::Decibels::gainToDecibels (m_level, -100.0f)), -36.0f, -10.0f, 0.0f, 1.0f);
        const float loudS = loud * loud * (3.0f - 2.0f * loud);
        if (m_evoSnap)
        {
            m_formation.formation = m_evo[roar::CreatureEvolution::formBase] * (1.0f - 0.85f * m_sleep);
            m_formation.fast = 0.0f;
        }
        m_formation.step (dt, m_jaw, loudS, m_roarDrive, m_sleep, m_evo[roar::CreatureEvolution::formBase],
                          m_evo[roar::CreatureEvolution::egg]);
        m_evoSnap = false;

        // Slow frames only once asleep and the cloud has finished dispersing (the formation has
        // settled on its sleeping target and no loud moment is still fading).
        const float base = m_evo[roar::CreatureEvolution::formBase];
        const float sleepTarget = juce::jlimit (0.0f, 1.0f, base + 0.35f * m_formation.fast * (1.0f - base)) * (1.0f - 0.85f * m_sleep);
        const bool settled = m_formation.fast < 0.01f && std::abs (m_formation.formation - sleepTarget) < 0.01f;
        m_idle.store (asleep && m_sleep > 0.98f && settled, std::memory_order_relaxed);

        // The clouds drift (offsets wrap hours apart, in double, so they never jump in a session).
        const double inky = m_evo[roar::CreatureEvolution::inkiness];
        const auto advance = [dt] (double& v, double rate) { v = std::fmod (v + rate * dt, c_driftWrap); };
        advance (m_driftBack[0], c_windX);
        advance (m_driftBack[1], c_windY + c_windInkY * inky);
        advance (m_driftFront[0], c_windX * 1.6);
        advance (m_driftFront[1], (c_windY + c_windInkY * inky) * 1.6 - 0.002);
        m_bloomPhase = std::fmod (m_bloomPhase + dt * juce::MathConstants<double>::twoPi / c_bloomSeconds, juce::MathConstants<double>::twoPi);

        // The slow noise scrolls (blot, edge bleed, mist, veins): rates integrated here, so turning
        // SHIMMER changes their speed and never their position, and each runs round a wide circle in
        // noise space, so it never wraps.
        const double lqRate = m_knobs.liquid;
        const double rates[c_numFlows] = { 0.02 * lqRate, 0.04 + 0.25 * lqRate, 0.04, 0.05 * lqRate };
        for (int i = 0; i < c_numFlows; ++i)
            m_flowPhase[i] = std::fmod (m_flowPhase[i] + dt * rates[i] / c_flowRadius, juce::MathConstants<double>::twoPi);
    }

    // An occasional blink while awake.
    m_blinkClock += dt;

    if (m_blinkClock > m_nextBlink + 0.18f)
    {
        m_blinkClock = 0.0f;
        m_nextBlink = 2.5f + 4.5f * m_random.nextFloat();
    }
}

void MonsterView::renderOpenGL()
{
    using namespace juce::gl;

    const auto now = Clock::now();
    const float dt = m_firstFrame ? 0.0f
                                  : juce::jlimit (0.0f, c_maxFrameSeconds, std::chrono::duration<float> (now - m_lastFrame).count());
    m_lastFrame = now;
    m_firstFrame = false;

    const float scale = (float) m_context.getRenderingScale();
    const int width = juce::roundToInt (scale * (float) m_width.load (std::memory_order_relaxed));
    const int height = juce::roundToInt (scale * (float) m_height.load (std::memory_order_relaxed));

    glViewport (0, 0, width, height);
    juce::OpenGLHelpers::clear (Theme::Colours::scope);

    if (m_program == nullptr || width <= 0 || height <= 0)
        return;

    updateAnimation (dt);

    const double seconds = std::fmod (std::chrono::duration<double> (now - m_startTime).count(), c_timeWrapSeconds);
    const float blinkPos = (m_blinkClock - m_nextBlink) / 0.18f;
    const float blink = (blinkPos > 0.0f && blinkPos < 1.0f) ? 1.0f - std::abs (2.0f * blinkPos - 1.0f) : 0.0f;

    glDisable (GL_DEPTH_TEST);
    glDisable (GL_BLEND);

    m_program->use();

    const auto set1 = [this] (Uniform u, float v)
    {
        if (m_uniforms[(size_t) u] >= 0)
            glUniform1f (m_uniforms[(size_t) u], v);
    };
    const auto setRgb = [this] (Uniform u, juce::Colour c)
    {
        if (m_uniforms[(size_t) u] >= 0)
            glUniform3f (m_uniforms[(size_t) u], c.getFloatRed(), c.getFloatGreen(), c.getFloatBlue());
    };

    if (m_uniforms[uRes] >= 0)
        glUniform2f (m_uniforms[uRes], (float) width, (float) height);

    set1 (uPxScale, scale);
    set1 (uRadius, m_cornerRadius.load (std::memory_order_relaxed) * scale);
    set1 (uTime, (float) seconds);
    set1 (uJaw, m_jaw);
    set1 (uGrowl, m_knobs.growl);
    set1 (uGrowlMod, m_growlMod);
    set1 (uLiquid, m_knobs.liquid);
    set1 (uLiquidPhase, safe (m_features.shimmerPhase.load (std::memory_order_relaxed), 0.0f, 1.0f));
    set1 (uRoar, m_knobs.roar);
    set1 (uRoarDrive, m_roarDrive);
    set1 (uSub, m_knobs.sub);
    set1 (uSubLevel, m_subLevel);
    set1 (uSubPhase, m_subPhase);
    set1 (uPitch, m_knobs.pitchSt);
    set1 (uSize, m_knobs.size);
    set1 (uGravelOn, m_gravelOn * (0.35f + 0.65f * m_knobs.gravel));
    set1 (uDetailLow, m_knobs.detailLow);
    set1 (uDetailHigh, m_knobs.detailHigh);
    set1 (uBoost, m_boost);
    set1 (uCut, m_cut);
    set1 (uSleep, m_sleep);
    set1 (uBlink, blink);
    set1 (uLevel, juce::jlimit (0.0f, 1.0f, m_level * 2.0f));

    if (m_uniforms[uBands] >= 0)
        glUniform1fv (m_uniforms[uBands], c_numBands, m_bands.data());

    set1 (uSwirlNotch, m_swirlNotch);
    set1 (uSwirlAmt, m_knobs.swirl);
    set1 (uEnsWidth, juce::jlimit (0.0f, 1.0f, m_ensWidth * (0.4f + 0.6f * m_knobs.ensemble)));
    set1 (uDeess, juce::jlimit (0.0f, 1.0f, m_deess));
    set1 (uTame, juce::jlimit (0.0f, 1.0f, m_tame));
    set1 (uGateClosed, juce::jlimit (0.0f, 1.0f, 1.0f - m_gate));
    set1 (uOvLevel, juce::jlimit (0.0f, 1.0f, m_ovLevel * (0.5f + m_knobs.overtone)));
    if (m_uniforms[uOvHarm] >= 0)
        glUniform1fv (m_uniforms[uOvHarm], (GLsizei) m_ovHarm.size(), m_ovHarm.data());
    set1 (uOvReplace, m_ovReplace);
    set1 (uRing, juce::jlimit (0.0f, 1.0f, m_ring));
    set1 (uRingFlicker, 0.5f + 0.5f * std::sin (m_ringPhase * juce::MathConstants<float>::twoPi));
    set1 (uRingBody, m_ringBody);
    set1 (uStrain, juce::jlimit (0.0f, 1.0f, m_strain));
    set1 (uStrainPulse, std::pow (0.5f + 0.5f * std::sin (m_strainPhase * juce::MathConstants<float>::twoPi), 3.0f));
    set1 (uClip, juce::jlimit (0.0f, 1.0f, m_clip));

    {
        using namespace Theme::Colours;
        setRgb (uColScope, scope);
        setRgb (uColGrid, grid);
        setRgb (uColOuter, juce::Colour (m_outsideArgb.load (std::memory_order_relaxed)));
        setRgb (uColPitch, pitch);
        setRgb (uColGrowl, growl);
        setRgb (uColLiquid, liquid);
        setRgb (uColRoar, Theme::Colours::roar); // qualified: "roar" is also the DSP namespace
        setRgb (uColSub, sub);
        setRgb (uColDetail, Theme::Colours::detail);
        setRgb (uColVocoder, vocoder);
        setRgb (uColOutput, output);
        setRgb (uColText, text);
        const auto accent = [] (::Params::ModuleId id) { return juce::Colour (::Params::module (id).accent); };
        setRgb (uColSwirl, accent (::Params::ModuleId::swirl));
        setRgb (uColOvertone, accent (::Params::ModuleId::overtone));
        setRgb (uColRing, accent (::Params::ModuleId::ring));
        setRgb (uColDeess, accent (::Params::ModuleId::deess));
        setRgb (uColEnsemble, accent (::Params::ModuleId::ensemble));
    }

    {
        float formation = m_formation.formation;
        float fog = roar::CreatureFormation::fogOpacity (m_evo[roar::CreatureEvolution::fogFront], m_formation.fast, m_sleep);
        const float testFormation = m_testFormation.load (std::memory_order_relaxed);
        const float testFog = m_testFog.load (std::memory_order_relaxed);
        formation = testFormation >= 0.0f ? juce::jlimit (0.0f, 1.0f, testFormation) : formation;
        fog = testFog >= 0.0f ? juce::jlimit (0.0f, 1.0f, testFog) : fog;

        const auto u = roar::CreatureUniforms::pack (m_evo, formation, fog, m_formation.fast, (float) m_bloomPhase);
        const auto set4 = [this] (Uniform id, const roar::CreatureUniforms::Vec4& v)
        {
            if (m_uniforms[(size_t) id] >= 0)
                glUniform4f (m_uniforms[(size_t) id], v[0], v[1], v[2], v[3]);
        };
        const auto set3 = [this] (Uniform id, const roar::CreatureUniforms::Vec3& v)
        {
            if (m_uniforms[(size_t) id] >= 0)
                glUniform3f (m_uniforms[(size_t) id], v[0], v[1], v[2]);
        };
        set4 (uEvoHead, u.head);
        set4 (uEvoWing, u.wing);
        set4 (uEvoHorn, u.horn);
        set4 (uEvoEyes, u.eyes);
        set4 (uEvoMaw, u.maw);
        set4 (uEvoInk, u.ink);
        set4 (uEvoFlow, u.flow);
        set4 (uEvoPal, u.pal);
        set4 (uEvoMisc, u.misc);
        set4 (uForm, u.form);
        set4 (uCloud, u.cloud);
        set4 (uSeed, u.seed);
        set4 (uSeedB, u.seedB);
        set4 (uDrift, { (float) m_driftBack[0], (float) m_driftBack[1], (float) m_driftFront[0], (float) m_driftFront[1] });
        // each scroll starts along its own axis (x for the veins, y for the rest)
        const auto along = [this] (int i, bool alongX) -> std::array<float, 2>
        {
            const float a = (float) (c_flowRadius * std::sin (m_flowPhase[i])), b = (float) (c_flowRadius * (1.0 - std::cos (m_flowPhase[i])));
            return alongX ? std::array<float, 2> { a, b } : std::array<float, 2> { b, a };
        };
        const auto blot = along (0, false), edge = along (1, false), mist = along (2, false), vein = along (3, true);
        set4 (uFlow, { blot[0], blot[1], edge[0], edge[1] });
        set4 (uFlow2, { mist[0], mist[1], vein[0], vein[1] });
        set3 (uColCloud, u.colCloud);
        set3 (uColCloudLit, u.colCloudLit);
        set3 (uColUnderglow, u.colUnderglow);
        set3 (uColRedInk, u.colRedInk);
        set3 (uColWashA, u.colWashA);
        set3 (uColWashB, u.colWashB);
        // No texture, no logo (the egg is skipped rather than drawn against an empty field).
        set1 (uEgg, m_logoTexture != 0 ? u.egg : 0.0f);
        set4 (uLogoBox, { 0.0f, 0.0f, c_logoBoxSize, 0.0f });
        if (m_uniforms[uLogoSdf] >= 0)
            glUniform1i (m_uniforms[uLogoSdf], 0);
    }

    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, m_logoTexture);

    if (m_vao != 0)
    {
        glBindVertexArray (m_vao);
    }
    else
    {
        glBindBuffer (GL_ARRAY_BUFFER, m_vbo);
        glEnableVertexAttribArray ((GLuint) m_positionAttrib);
        glVertexAttribPointer ((GLuint) m_positionAttrib, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    }

    glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);

    if (m_captureRequested.exchange (false, std::memory_order_relaxed))
    {
        std::vector<juce::uint8> pixels ((size_t) (width * height * 4));
        glReadPixels (0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        juce::Image image (juce::Image::RGB, width, height, false);
        {
            juce::Image::BitmapData data (image, juce::Image::BitmapData::writeOnly);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                {
                    const auto* px = pixels.data() + ((size_t) (height - 1 - y) * (size_t) width + (size_t) x) * 4;
                    data.setPixelColour (x, y, juce::Colour (px[0], px[1], px[2]));
                }
        }
        const juce::ScopedLock lock (m_captureLock);
        m_capture = image;
    }

    // Leave the state as JUCE expects it for the component overlay.
    if (m_vao != 0)
    {
        glBindVertexArray (0);
    }
    else
    {
        glDisableVertexAttribArray ((GLuint) m_positionAttrib);
        glBindBuffer (GL_ARRAY_BUFFER, 0);
    }

    glBindTexture (GL_TEXTURE_2D, 0);
    glUseProgram (0);
}
