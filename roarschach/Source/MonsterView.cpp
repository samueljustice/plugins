#include "MonsterView.h"

#include <cmath>

namespace
{
    constexpr float c_quietLevel = 0.001f;      // -60 dBFS: below this the input counts as silence
    constexpr float c_sleepAfterSeconds = 1.5f;
    // Hosts stop calling processBlock when the transport stops: the features then freeze at their
    // last values. No new chunk for this long counts as silence (longer than any host buffer).
    constexpr float c_staleAfterSeconds = 0.5f;
    constexpr int c_frameHz = 30;                // GL frames per second while awake
    constexpr int c_idleFrameDivider = 3;        // 10 Hz once asleep and settled (still notices sound coming back)
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

float smin (float a, float b, float k)
{
    float h = clamp (0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return mix (b, a, h) - k * h * (1.0 - h);
}

float sdEllipse (vec2 p, vec2 r)
{
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
    float h = clamp (dot (pa, ba) / dot (ba, ba), 0.0, 1.0);
    return length (pa - ba * h) - mix (ra, rb, h);
}

mat2 rot (float a)
{
    float c = cos (a);
    float s = sin (a);
    return mat2 (c, s, -s, c);
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

    // ---- the card: dark scope, faint grid, a fold down the middle, paper fibre, vignette
    vec3 col = uColScope;
    vec2 gp = p + heat * awake * 0.004 * vec2 (sin (p.y * 60.0 + t * 9.0), sin (p.x * 47.0 - t * 7.0));
    vec2 gd = abs (fract (gp * 8.0 + 0.5) - 0.5) / (8.0 * px);
    float grid = 1.0 - smoothstep (0.0, 1.25, min (gd.x, gd.y));
    col = mix (col, uColGrid, grid * 0.45);
    col += uColGrid * exp (-abs (p.x) / (2.0 * px)) * 0.5;
    col += (vnoise (frag * 0.45) - 0.5) * 0.010;
    vec2 vg = p * vec2 (0.42, 1.25);
    col *= 1.0 - 0.45 * dot (vg, vg);

    // ---- mirror: the blot is the same on both halves
    vec2 q = vec2 (abs (p.x), p.y);

    float breath = sin (t * mix (1.6, 0.85, uSleep));
    float scale = 1.0 + clamp (-uPitch / 12.0, -1.0, 1.0) * 0.09 + 0.014 * breath * (0.35 + uSleep)
                + uJaw * 0.025 * awake;
    float shake = uGrowl * uGrowlMod * 0.006 * (0.3 + uJaw) * awake;
    vec2 h = (q - vec2 (0.0, 0.025 + shake)) / scale;
    float aa = 1.5 * px / scale;

    // liquid: the whole skin wobbles
    vec2 wob = vec2 (sin (h.y * 17.0 + t * 1.9 + lph), sin (h.x * 13.0 + h.y * 6.0 + t * 1.4 - lph * 0.5));
    h += wob * lq * 0.010;

    // ---- mouth geometry (shared by the silhouette, the maw and the teeth)
    float mY = -0.075;
    float mw = 0.15 + 0.08 * size;
    float open = (0.006 + uJaw * (0.12 + 0.05 * size)) * awake;
    open = max (open + uGrowl * uGrowlMod * 0.014 * uJaw * awake, 0.003);
    float chinDrop = open * 0.85;

    // ---- silhouette
    float dCran = sdEllipse (h - vec2 (0.0, 0.10), vec2 (0.29, 0.26));
    float dCheek = sdEllipse (h - vec2 (0.12 + 0.05 * size, -0.06), vec2 (0.20 + 0.05 * size, 0.15));
    float dJaw = sdTaper (h, vec2 (0.22 + 0.06 * size, -0.02), vec2 (0.09 + 0.04 * size, -0.21 - chinDrop), 0.06, 0.07);
    float dChin = sdEllipse (h - vec2 (0.0, -0.18 - chinDrop), vec2 (0.15 + 0.07 * size, 0.11));
    float dHorn = min (sdTaper (h, vec2 (0.19, 0.22), vec2 (0.35, 0.29), 0.055, 0.032),
                       sdTaper (h, vec2 (0.35, 0.29), vec2 (0.49, 0.40), 0.032, 0.004));
    float dSpike = min (sdTaper (h, vec2 (0.27 + 0.05 * size, -0.06), vec2 (0.39 + 0.07 * size, -0.12), 0.03, 0.003),
                        sdTaper (h, vec2 (0.24 + 0.05 * size, -0.14 - chinDrop * 0.5), vec2 (0.32 + 0.06 * size, -0.25 - chinDrop * 0.6), 0.025, 0.003));
    float dHead = smin (dCran, dCheek, 0.07);
    dHead = smin (dHead, dJaw, 0.05);
    dHead = smin (dHead, dChin, 0.06);
    dHead = smin (dHead, dHorn, 0.04);
    dHead = smin (dHead, dSpike, 0.025);

    // the blot's wings spread sideways, swelling a little with the output
    float swell = 1.0 + 0.06 * uLevel * awake + 0.02 * breath;
    // (an envelope cut by noise, so the wings read as pressed ink rather than shapes)
    vec2 wq = h;
    float env = sdEllipse (rot (-0.10) * (wq - vec2 (0.80, 0.0)), vec2 (0.60, 0.25) * swell);
    float blotN = fbm (wq * vec2 (2.4, 3.6) + vec2 (4.0, 1.3) + vec2 (0.0, t * 0.02 + lph * 0.03) * lq);
    float dWing = -smin (-env, (blotN - 0.50) * 0.22, 0.04);
    dWing = smin (dWing, sdEllipse (rot (-0.35) * (wq - vec2 (0.50, 0.07)), vec2 (0.21, 0.085) * swell), 0.11);
    dWing = smin (dWing, sdEllipse (rot (0.30) * (wq - vec2 (0.66, -0.17)), vec2 (0.15, 0.05) * swell), 0.10);
    dWing = min (dWing, length (wq - vec2 (1.32, -0.04)) - 0.035);
    dWing = min (dWing, length (wq - vec2 (1.44, 0.15)) - 0.022);
    dWing = min (dWing, length (wq - vec2 (0.96, 0.31)) - 0.030);
    dWing = min (dWing, length (wq - vec2 (1.24, -0.26)) - 0.024);
    dWing = min (dWing, length (wq - vec2 (0.50, 0.36)) - 0.018);

    float dInk = smin (dHead, dWing + 0.010, 0.12);

    // ink tendrils dripping off the underside, longer with liquid
    float drip = smoothstep (0.03, 0.45, lq);
    vec4 dx = vec4 (0.06, 0.25, 0.56, 0.86);
    vec4 dtop = vec4 (-0.27 - chinDrop, -0.22 - chinDrop * 0.6, -0.03, -0.14);
    float dDrip = 1.0e3;
    for (int i = 0; i < 4; i++)
    {
        float fi = float (i);
        float x = dx.x;
        float top = dtop.x;
        if (i == 1) { x = dx.y; top = dtop.y; }
        if (i == 2) { x = dx.z; top = dtop.z; }
        if (i == 3) { x = dx.w; top = dtop.w; }
        float life = fract (t * (0.06 + 0.035 * fi) + fi * 0.37);
        float len = (0.02 + 0.17 * life) * drip;
        vec2 tip = vec2 (x + 0.008 * sin (t * 1.3 + fi * 2.0), top - len);
        float d = sdTaper (h, vec2 (x, top + 0.03), tip, 0.018 * drip, (0.005 + 0.006 * life) * drip);
        d = min (d, length (h - tip) - (0.008 + 0.010 * life) * drip);
        float fall = fract (t * 0.33 + fi * 0.61);
        vec2 drop = vec2 (tip.x, top - 0.19 * drip - fall * 0.5);
        d = min (d, length ((h - drop) * vec2 (1.0, 0.8)) - 0.010 * drip * (1.0 - fall));
        dDrip = min (dDrip, d);
    }
    dInk = smin (dInk, dDrip, 0.03);

    // vocoder frill: a fan of 32 ridges under the jaw (even bands on the left, odd on the right,
    // low bands in the middle), each spike as long as its band is loud
    vec2 fc = vec2 (0.0, -0.13 - chinDrop * 0.6);
    vec2 fv = h - fc;
    float fr = length (fv);
    float fang = atan (fv.x, -fv.y);
    float sector = clamp (fang / 1.75, 0.0, 0.9999) * 16.0;
    float fk = floor (sector);
    float fu = fract (sector) - 0.5;
    // clamped as a float (no integer clamp in 1.10) so a degenerate angle can't index out of range
    float bandSlot = clamp (fk * 2.0 + (p.x > 0.0 ? 1.0 : 0.0), 0.0, 31.0);
    float band = clamp (uBands[int (bandSlot)], 0.0, 1.0);
    float r0 = 0.17 + 0.04 * size;
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
    dFrill += (1.0 - uGravelOn) * 0.05;
    dInk = smin (dInk, dFrill, 0.015);

    // inky edges: slow-moving noise bleeds the outline, more so out on the wings and with liquid
    float edgeN = fbm (h * 6.0 + vec2 (0.0, t * (0.04 + 0.25 * lq)));
    dInk += (edgeN - 0.5) * (0.020 + 0.030 * lq + 0.045 * smoothstep (0.35, 1.3, h.x));
    // the noisy parts are not true distances: rescale by the gradient so edges and halos keep their width
    // (only near the edge: inside, the per-quad gradient would show as steps in the shading)
    float grad = length (vec2 (dFdx (dInk), dFdy (dInk))) * scale / px;
    float dEdge = dInk / mix (1.0, clamp (grad, 0.25, 1.5), smoothstep (-0.04, -0.01, dInk));
    float bleed = aa + 0.002 * edgeN;
    float ink = 1.0 - smoothstep (-bleed, bleed, dEdge);
    float feather = exp (-max (dEdge, 0.0) * 160.0) * smoothstep (0.55, 0.95, vnoise (frag * 0.35 / uPxScale)) * 0.5;

    // ---- height field for wet-ink shading (normals from screen derivatives)
    float H = 0.55 * (1.0 - exp (-max (-dInk, 0.0) * 9.0)) + 0.35 * (1.0 - exp (-max (-dHead, 0.0) * 5.0));
    float dBrow = sdTaper (h, vec2 (0.03, 0.118), vec2 (0.25, 0.205), 0.018, 0.030);
    float dBrow2 = sdTaper (h, vec2 (0.06, 0.185), vec2 (0.21, 0.265), 0.012, 0.020);
    float dCheekBone = sdTaper (h, vec2 (0.15, -0.005), vec2 (0.28, 0.035), 0.016, 0.010);
    float dNose = sdTaper (h, vec2 (0.0, 0.12), vec2 (0.0, 0.0), 0.022, 0.032);
    H += 0.38 * smoothstep (0.02, -0.015, dBrow) + 0.22 * smoothstep (0.015, -0.01, dBrow2);
    H += 0.18 * smoothstep (0.02, -0.01, dCheekBone) + 0.15 * smoothstep (0.02, -0.01, dNose);
    float dNostril = sdEllipse (rot (0.5) * (h - vec2 (0.034, -0.012)), vec2 (0.014, 0.006));
    H -= 0.25 * smoothstep (0.006, -0.004, dNostril);
    // skin: soft lumps for the light, thin veins only as colour (as bumps they alias into outlines)
    // (rotated and warped: a plain value-noise contour on the grid draws rounded boxes, not veins)
    vec2 vp = rot (0.6) * h * 16.0 + vec2 (t * 0.05 * lq, lph * 0.15);
    vp += 1.6 * vec2 (vnoise (h * 7.0 + vec2 (5.2, 1.3)), vnoise (h * 7.0 + vec2 (2.8, 9.1)));
    float veinN = abs (vnoise (vp) - 0.5);
    float veins = (1.0 - smoothstep (0.0, 0.06, veinN)) * smoothstep (0.0, -0.04, dInk);
    H += (vnoise (h * 22.0) - 0.5) * (0.04 + 0.06 * uDetailHigh + 0.06 * uCut);
    float ripple = sin (length (h - vec2 (0.0, 0.05)) * 70.0 - t * 4.0 - lph);
    H += lq * 0.05 * ripple * smoothstep (0.0, -0.05, dInk);

    // eyes: slanted almonds under the brows, sockets sunk into the skin
    vec2 ec = vec2 (0.135, 0.083);
    vec2 e = rot (-0.38) * (h - ec);
    float ew = 0.060;
    float eyeOpen = awake * (1.0 - uBlink);
    float eh = 0.032 * eyeOpen + 0.0012;
    float pointed = 1.0 - 0.55 * clamp (abs (e.x) / ew, 0.0, 1.0);
    float dEye = sdEllipse (vec2 (e.x, e.y / pointed), vec2 (ew, eh));
    float dSocket = sdEllipse (e - vec2 (0.0, -0.004), vec2 (ew * 1.3, 0.045));
    H -= 0.32 * smoothstep (0.02, -0.02, dSocket);

    // maw
    float mx = h.x / mw;
    float mx2 = mx * mx;
    float lipCurve = sqrt (max (1.0 - mx2, 0.0));
    float yU = mY + 0.016 * (1.0 - mx2) - 0.014 * mx2;
    float yL = mY - 0.014 * mx2 - 0.002 * (1.0 - mx2) - open * lipCurve;
    float dMouth = max (h.x - mw, max (h.y - yU, yL - h.y));
    float dLips = sdEllipse ((h - vec2 (0.0, mY - open * 0.5)) / vec2 (1.0, 1.0 + open * 6.0), vec2 (mw * 1.08, 0.03));
    H += 0.12 * smoothstep (0.015, -0.01, dLips);

    vec2 dH = vec2 (dFdx (H), dFdy (H)) / (px * 2.0);
    vec3 n = normalize (vec3 (-dH.x * 0.03, -dH.y * 0.03, 1.0));
    vec3 L = normalize (vec3 (0.0, 0.55, 0.85));
    float diff = max (dot (n, L), 0.0);
    vec3 halfV = normalize (L + vec3 (0.0, 0.0, 1.0));
    float specFlat = pow (halfV.z, 30.0);   // flat ink gets no highlight, only the ridges and swells do
    float spec = max (pow (max (dot (n, halfV), 0.0), 30.0) - specFlat, 0.0) / (1.0 - specFlat);

    // ---- glows behind the ink: sub rings, embers, the rim halo
    vec2 mc = vec2 (0.0, (mY - open * 0.4) * scale + 0.025);
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
    vec2 cell = floor (ep);
    vec2 fl = fract (ep) - 0.5;
    float rnd = hash12 (cell);
    vec2 off = vec2 (hash12 (cell + 7.1), hash12 (cell + 3.3)) - 0.5;
    off.x += 0.22 * sin (t * 2.0 + rnd * 6.28);
    float de = length (fl - off * 0.6);
    float embers = step (0.70, rnd) * exp (-de * de * 380.0) * (0.55 + 0.45 * sin (t * 6.0 + rnd * 40.0));
    embers *= smoothstep (1.25, 0.15, length (p * vec2 (0.55, 1.0)));
    col += uColRoar * embers * heat * 2.2 * awake;

    vec3 rim = mix (uColPitch, uColLiquid, smoothstep (0.0, 0.8, lq));
    rim = mix (rim, uColRoar, heat * 0.85);
    float haloAmt = 0.07 + 0.30 * uLevel * awake + 0.12 * heat;
    float halo = exp (-max (dEdge, 0.0) * 34.0) * (1.0 - ink);
    col += rim * halo * haloAmt;
    col = mix (col, vec3 (0.02, 0.02, 0.03), feather * (1.0 - ink));

    // ---- the ink itself
    float density = mix (1.0, 0.90 + 0.10 * edgeN, smoothstep (0.35, 0.9, h.x));
    vec3 inkCol = vec3 (0.020, 0.022, 0.034) + uColPitch * 0.035 * edgeN;
    inkCol += uColLiquid * veins * (0.008 + 0.025 * lq + 0.02 * uDetailHigh) + uColRoar * veins * heat * 0.05;
    inkCol *= 0.45 + 1.6 * diff * diff;
    inkCol += mix (uColLiquid, uColText, 0.6) * spec * (0.18 + 0.40 * lq + 0.15 * uLevel);
    float rimIn = smoothstep (-0.03, 0.0, dEdge);
    inkCol += rim * rimIn * (0.06 + 0.20 * uLevel * awake);
    float grain = hash12 (floor (frag) + floor (t * 24.0)) - 0.5;
    inkCol += grain * (0.012 + 0.05 * uBoost + 0.025 * uDetailLow);
    float speck = step (0.9985 - 0.004 * uBoost - 0.001 * uDetailLow, hash12 (floor (frag * 0.5) + 17.0));
    inkCol += uColDetail * speck * (0.10 + 0.4 * uBoost) * smoothstep (0.0, -0.02, dInk);
    col = mix (col, inkCol, ink * density);

    // frill glow
    float frillCore = (1.0 - smoothstep (0.0, halfW, abs (fu))) * step (r0 - 0.02, fr) * step (fr, r0 + flen + 0.01);
    frillCore *= step (fang, 1.75);
    float frillGlow = exp (-max (dFrill, 0.0) * 55.0);
    col += uColVocoder * uGravelOn * (frillCore * (0.25 + 1.4 * band) * (1.0 - along * 0.6) + frillGlow * band * 0.35);

    // eyes
    float eye = 1.0 - smoothstep (-aa, aa, dEye);
    vec3 eyeCol = mix (uColOutput, uColRoar, clamp (heat * 1.3, 0.0, 1.0));
    float eI = (0.75 + 1.7 * heat + 0.5 * uLevel) * awake + 0.12 * uSleep;
    float iris = exp (-length (e / vec2 (ew * 0.8, 0.032)) * 2.0);
    float slit = 1.0 - smoothstep (0.003, 0.007 + 0.004 * (1.0 - heat), abs (h.x - ec.x));
    vec3 eyeFill = eyeCol * (0.35 + 1.2 * iris) * eI;
    eyeFill = mix (eyeFill, vec3 (0.02, 0.0, 0.0), slit * eyeOpen * 0.9);
    eyeFill += vec3 (1.0, 0.95, 0.8) * pow (iris, 6.0) * 0.5 * eI;
    col = mix (col, eyeFill, eye);
    col += eyeCol * exp (-max (dEye, 0.0) * 32.0) * eI * 0.30 * (1.0 - eye);
    float lid = exp (-abs (e.y + 0.006 * (1.0 - (e.x * e.x) / (ew * ew))) / 0.0025) * (1.0 - smoothstep (ew * 0.8, ew, abs (e.x)));
    col += eyeCol * lid * 0.45 * uSleep;

    // maw: a hot throat behind the teeth
    float mouth = 1.0 - smoothstep (-aa, aa, dMouth);
    float subPulse = uSub * uSubLevel * (0.5 + 0.5 * sin (uSubPhase * 6.2832 * 3.0));
    float tg = exp (-length ((h - vec2 (0.0, mY - open * 0.5)) / vec2 (mw * 0.75, open * 0.6 + 0.015)) * 1.5);
    float meanBand = 0.0;
    for (int i = 0; i < 32; i += 4)
        meanBand += uBands[i] * 0.125;
    vec3 throatCol = mix (vec3 (0.20, 0.015, 0.03), uColRoar, 0.35 + 0.65 * heat);
    throatCol = mix (throatCol, uColVocoder, uGravelOn * clamp (meanBand * 1.5, 0.0, 0.7));
    vec3 throat = vec3 (0.05, 0.004, 0.01) + throatCol * tg * (0.35 + 1.3 * heat + 0.6 * uLevel + 0.8 * subPulse) * awake;
    float lipShade = smoothstep (0.0, 0.02, min (yU - h.y, h.y - yL));
    throat *= 0.35 + 0.65 * lipShade;
    throat += uColGrowl * (1.0 - lipShade) * uGrowl * abs (uGrowlMod) * 0.6 * awake;
    col = mix (col, throat, mouth);

    // teeth: two rows behind, the main rows on the lips (they interlock when the jaw is shut)
    float tremor = uGrowl * awake * (0.0025 * sin (t * 83.0) + 0.002 * uGrowlMod) * (0.3 + uJaw);
    float tw = mw / 5.0;
    float teethZone = 1.0 - smoothstep (mw * 0.92, mw * 1.0, h.x);
    float baseLen = 0.024 + 0.022 * size;
    vec3 bone = mix (vec3 (0.80, 0.75, 0.66), uColText, 0.3);
    vec3 glowOnBone = throatCol * tg * (0.4 + heat) * awake;

    float u3 = h.x / (tw * 0.72) + 0.25;
    float f3 = fract (u3) - 0.5;
    float tc3 = 1.0 - pow (abs (f3) * 2.0, 1.3);
    float back = max ((yU - 0.006 - baseLen * 0.55 * tc3) - h.y, h.y - yU);
    back = min (back, max (h.y - (yL + 0.004 + baseLen * 0.45 * tc3), yL - h.y));
    float backTeeth = (1.0 - smoothstep (-fwidth (back), fwidth (back), back)) * mouth * teethZone;
    col = mix (col, bone * 0.32 + glowOnBone * 0.4, backTeeth);

    float u2 = h.x / tw + 0.5;
    float k2 = floor (u2);
    float f2 = fract (u2) - 0.5;
    float tc2 = 1.0 - pow (abs (f2) * 2.0, 1.25);
    float fang2 = abs (k2 - 1.0) < 0.5 ? 1.5 : 1.0 - 0.07 * k2;
    float L2 = baseLen * 0.8 * fang2;
    float yLt = yL - tremor;
    float dLow = max (h.y - (yLt + L2 * tc2), (yLt - 0.008) - h.y);
    float lowTeeth = (1.0 - smoothstep (-fwidth (dLow), fwidth (dLow), dLow)) * teethZone;
    float lowShade = clamp ((h.y - yLt) / max (L2, 1.0e-3), 0.0, 1.0);
    vec3 lowCol = bone * (0.45 + 0.55 * lowShade) * (0.75 + 0.25 * tc2) + glowOnBone * 0.5;
    col = mix (col, lowCol, lowTeeth * smoothstep (0.0, 0.02, -dInk + 0.02));

    float u1 = h.x / tw;
    float k1 = floor (u1);
    float f1 = fract (u1) - 0.5;
    float tc1 = 1.0 - pow (abs (f1) * 2.0, 1.25);
    float fang1 = abs (k1 - 1.0) < 0.5 ? 1.95 : (abs (k1 - 3.0) < 0.5 ? 1.3 : 1.0 - 0.08 * k1);
    float L1 = baseLen * fang1;
    float yUt = yU + tremor;
    float dUp = max ((yUt - L1 * tc1) - h.y, h.y - (yUt + 0.008));
    float upTeeth = (1.0 - smoothstep (-fwidth (dUp), fwidth (dUp), dUp)) * teethZone;
    float upShade = clamp ((yUt - h.y) / max (L1, 1.0e-3), 0.0, 1.0);
    vec3 upCol = bone * (0.5 + 0.5 * upShade) * (0.75 + 0.25 * tc1) + glowOnBone * 0.35;
    upCol += vec3 (1.0) * pow (tc1, 8.0) * upShade * 0.12;
    col = mix (col, upCol, upTeeth * smoothstep (0.0, 0.02, -dInk + 0.02));

    // rings and embers also play faintly over the creature
    col += uColSub * rings * subAmt * 0.18 * ink;
    col += uColRoar * embers * heat * 0.8 * awake * ink;

    // asleep: a slow mist settles over the lower half
    if (uSleep > 0.001)
    {
        float mist = fbm (vec2 (p.x * 1.6 + t * 0.03, p.y * 3.2 - t * 0.05));
        mist *= smoothstep (0.25, -0.5, p.y);
        col += vec3 (0.20, 0.23, 0.30) * mist * mist * 0.6 * uSleep;
    }

    col = col / (1.0 + col * 0.22);
    col = clamp (col, 0.0, 1.0);

    // rounded corners, filled with what is around the view
    vec2 cq = abs (frag - 0.5 * uRes) - (0.5 * uRes - vec2 (uRadius));
    float dc = length (max (cq, 0.0)) - uRadius;
    col = mix (col, uColOuter, smoothstep (-0.75, 0.75, dc));

    gl_FragColor = vec4 (col, 1.0);
}
)GLSL";

    const char* const c_uniformNames[] =
    {
        "uRes", "uPxScale", "uRadius", "uTime", "uJaw", "uGrowl", "uGrowlMod", "uLiquid", "uLiquidPhase", "uRoar",
        "uRoarDrive", "uSub", "uSubLevel", "uSubPhase", "uPitch", "uSize", "uGravelOn", "uDetailLow", "uDetailHigh",
        "uBoost", "uCut", "uSleep", "uBlink", "uLevel", "uBands",
        "uColScope", "uColGrid", "uColOuter", "uColPitch", "uColGrowl", "uColLiquid", "uColRoar", "uColSub",
        "uColDetail", "uColVocoder", "uColOutput", "uColText"
    };
}

MonsterView::MonsterView (const roar::MonsterFeatures& features)
    : m_features (features)
{
    static_assert (std::size (c_uniformNames) == numUniforms, "one name per uniform");

    // Not opaque: the overlay (readouts, fallback text) is painted over the GL frame.
    setOpaque (false);
    setInterceptsMouseClicks (false, false);

    m_context.setOpenGLVersionRequired (juce::OpenGLContext::openGL3_2);
    m_context.setRenderer (this);
    m_context.setComponentPaintingEnabled (true);
    // No continuous repainting: the shader is a full-screen procedural one, so drawing it at the
    // display rate (up to 120 Hz) costs a lot of GPU for no visible gain. The timer below asks for
    // frames at 30 Hz, and at 10 Hz once the creature is asleep.
    m_context.setContinuousRepainting (false);
    m_context.attachTo (*this);

    // The timer also refreshes the overlay text, which only repaints when it changes.
    startTimerHz (c_frameHz);
}

MonsterView::~MonsterView()
{
    stopTimer();
    m_context.detach();
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

    m_shaderFailed.store (false, std::memory_order_relaxed);
    m_glReady.store (true, std::memory_order_relaxed);
}

void MonsterView::openGLContextClosing()
{
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

    m_vbo = 0;
    m_vao = 0;
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
    m_idle.store (asleep && m_sleep > 0.98f, std::memory_order_relaxed);

    follow (m_jaw, safe (load (f.jaw), 0.0f, 1.0f), dt, 0.012f, 0.060f);
    follow (m_growlMod, safe (load (f.growlMod), -1.0f, 1.0f), dt, 0.008f, 0.008f);
    follow (m_roarDrive, safe (load (f.roarDrive), 0.0f, 1.0f), dt, 0.060f, 0.25f);
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
    knob (m_knobs.pitchSt, m_pitchSt, -12.0f, 12.0f);
    knob (m_knobs.size, m_size, 0.0f, 1.0f);
    knob (m_knobs.growl, m_growl, 0.0f, 1.0f);
    knob (m_knobs.liquid, m_liquid, 0.0f, 1.0f);
    knob (m_knobs.roar, m_roar, 0.0f, 1.0f);
    knob (m_knobs.sub, m_sub, 0.0f, 1.0f);
    knob (m_knobs.gravel, m_gravel, 0.0f, 1.0f);
    knob (m_knobs.detailLow, m_detailLow, 0.0f, 1.0f);
    knob (m_knobs.detailHigh, m_detailHigh, 0.0f, 1.0f);

    // Rings travel faster while the sub is loud.
    m_subPhase = std::fmod (m_subPhase + dt * (0.30f + 0.9f * m_subLevel), 1.0f);

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
    set1 (uLiquidPhase, safe (m_features.liquidPhase.load (std::memory_order_relaxed), 0.0f, 1.0f));
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
    }

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

    glUseProgram (0);
}
