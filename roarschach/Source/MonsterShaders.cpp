// The creature view's GLSL, in chunks (see MonsterShaders.h). Written for GLSL 1.10 (legacy contexts)
// so that OpenGLHelpers::translate*ToV3 turns it into valid 1.50 core code: float literals
// everywhere, no round/tanh, no const arrays, constant loop bounds with break, one output per
// pass, and none of the words the translation rewrites inside names or comments.
#include "MonsterShaders.h"

namespace roar::shaders
{
namespace
{
const char* const c_vertex = R"GLSL(
attribute vec2 aPosition;
varying vec2 vUv;

void main()
{
    vUv = aPosition * 0.5 + 0.5;
    gl_Position = vec4 (aPosition, 0.0, 1.0);
}
)GLSL";

const char* const c_creatureUniforms = R"GLSL(
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
uniform vec4 uSeedB;     // the blot being glided to xy, glide 0..1, the stage 0..1
uniform vec4 uEvoChar;   // character leans: fluid, beast, eldritch, deep (-1/3..1, 0 neutral)
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
uniform vec4 uHead;      // head offset y, scale add, the lagged offset (wings), breath
uniform float uLip;      // lip-only syllable detail, -1..1
uniform float uJawVel;   // the jaw spring's speed
uniform float uRoarHold; // seconds of roar hold left
uniform float uAspect;   // width / height
uniform vec4 uVort[8];   // vortices: view x, y, core radius, signed peak speed (view heights per second)
uniform vec4 uAtmo;      // the top mist: opacity (eased), drift xy (view heights), on
uniform vec3 uAtmoTint;  // the stage's hue lean within the cool palette (near white)
uniform vec4 uMood;      // the colour mood: the palette's hue turn on the YIQ chroma plane, (s cos a - 1, s sin a), how warm it is (0..1); 0 = none
)GLSL";

const char* const c_atmo = R"GLSL(
// ---- the atmosphere: a high-key, cool, desaturated blue-grey value ramp (sRGB), dark to light,
// each stop placed at its own luma, so atmoRamp (v) has a luma of about v
vec3 atmoRamp (float v)
{
    vec3 c = vec3 (0.059, 0.090, 0.141) * clamp (v / 0.086, 0.0, 1.0);
    c = mix (c, vec3 (0.145, 0.192, 0.247), clamp ((v - 0.086) / 0.098, 0.0, 1.0));
    c = mix (c, vec3 (0.325, 0.392, 0.459), clamp ((v - 0.184) / 0.195, 0.0, 1.0));
    c = mix (c, vec3 (0.455, 0.549, 0.627), clamp ((v - 0.379) / 0.150, 0.0, 1.0));
    c = mix (c, vec3 (0.580, 0.690, 0.757), clamp ((v - 0.529) / 0.135, 0.0, 1.0));
    c = mix (c, vec3 (0.725, 0.827, 0.882), clamp ((v - 0.664) / 0.138, 0.0, 1.0));
    c = mix (c, vec3 (0.859, 0.925, 0.961), clamp ((v - 0.802) / 0.107, 0.0, 1.0));
    c = mix (c, vec3 (0.950, 0.980, 0.995), clamp ((v - 0.909) / 0.090, 0.0, 1.0));
    return c;
}

// The colour mood (from the settings): the palette's chroma turned and scaled on the YIQ chroma plane,
// its luma kept, so the ramp keeps its values and only leans towards ember, electric blue, violet or
// lichen. Exactly the input at uMood = 0.
vec3 moodTurn (vec3 c)
{
    vec2 iq = vec2 (dot (c, vec3 (0.596, -0.274, -0.322)), dot (c, vec3 (0.211, -0.523, 0.312)));
    vec2 d = vec2 (uMood.x * iq.x - uMood.y * iq.y, uMood.y * iq.x + uMood.x * iq.y);
    return c + d.x * vec3 (0.956, -0.272, -1.106) + d.y * vec3 (0.621, -0.647, 1.703);
}

// A value on the ramp, with the stage's lean and the mood
vec3 atmoTone (float v)
{
    return moodTurn (atmoRamp (v) * uAtmoTint);
}

// The warm accents against a warm mood: their own chroma turned towards a hotter yellow-orange and
// strengthened, so the eyes and throat stay a colour apart from ember or stone clouds. Exactly the
// input when the mood is not warm.
vec3 accentHeat (vec3 chroma)
{
    float a = -0.45 * uMood.z, k = 1.0 + 0.7 * uMood.z;
    vec2 iq = vec2 (dot (chroma, vec3 (0.596, -0.274, -0.322)), dot (chroma, vec3 (0.211, -0.523, 0.312)));
    vec2 r = vec2 (cos (a) * iq.x - sin (a) * iq.y, sin (a) * iq.x + cos (a) * iq.y) * k - iq;
    return chroma + r.x * vec3 (0.956, -0.272, -1.106) + r.y * vec3 (0.621, -0.647, 1.703);
}

// The scene (tone-mapped, 0..1) re-graded onto the ramp: its values lifted high-key, its colour
// replaced by the ramp's, except a softened share kept by the warm accents (eyes, throat, red ink)
vec3 atmoGrade (vec3 col, float l)
{
    vec3 chroma = col - vec3 (l);
    float warm = clamp ((col.r - col.b) * 3.0, 0.0, 1.0);
    // (warm light is lifted less: a glow keeps its colour and never turns into a pale disc)
    float v = 0.16 + mix (1.25 - 0.30 * l, 0.95 - 0.10 * l, 0.6 * warm) * l;
    vec3 g = atmoTone (v);
    // (under a warm mood a warm accent sits on a little less of the mood's turn: a cooler ground)
    g = mix (g, atmoRamp (v) * uAtmoTint, 0.35 * warm * uMood.z);
    g += accentHeat (chroma) * (0.10 + 0.40 * warm);
    return max (g, vec3 (0.0));
}

// The hue of a warm light as a small offset (no value of its own): what is left of a glow seen
// through the mist. Zero for cool lights.
vec3 warmChroma (vec3 c)
{
    vec3 n = c / max (max (c.r, c.g), max (c.b, 1.0e-3));
    return (n - vec3 (dot (n, vec3 (0.3, 0.59, 0.11)))) * step (c.b, c.r);
}

// A warm light seen through the mist: the mist's own value, the light's hue (w: how much, 0..1)
vec3 warmVeil (vec3 col, vec3 light, float w)
{
    float l = dot (col, vec3 (0.3, 0.59, 0.11));
    return mix (col, vec3 (l) + accentHeat (warmChroma (light)) * 0.18, clamp (w, 0.0, 1.0) * step (light.b, light.r));
}

// The mist's own colour for a value: the ramp, the stage's lean, a faint mauve in some mid-tones
vec3 atmoMistCol (float v, float mauve)
{
    vec3 c = atmoTone (v);
    c += vec3 (0.018, -0.010, 0.012) * mauve * exp (-((v - 0.55) * (v - 0.55)) / 0.03);
    return c;
}
)GLSL";

// The top mist, the same in every tier: the baked noise at the same scales and constants, so a tier
// change never swaps one sky for another (only the creature's shadow in it differs by tier).
const char* const c_mist = R"GLSL(
// x: the mist's soft value noise (-1..1), y: its long stroke layer (0..1), z: where it leans mauve
vec3 mistField (vec2 p, vec2 warp)
{
    vec2 mq = p + uAtmo.yz + warp;
    float mA = texture2D (uNoise, mq * 0.21 + vec2 (0.13, 0.71)).r;
    float mB = texture2D (uNoise, mat2 (0.8, 0.6, -0.6, 0.8) * mq * 0.53 + vec2 (0.57, 0.29)).r;
    // the long strokes: bent by a slow layer of their own and by the soft one, so their slant wanders
    // across the view (never a set of parallel diagonals), and only in patches (they fade where the
    // soft layer is thin, so none runs the whole width)
    // (the bending layer varies mostly across the view: varying up it would squeeze the strokes into threads)
    float mC = texture2D (uNoise, mq * vec2 (0.16, 0.05) + vec2 (0.37, 0.19)).r;
    vec2 sq = mat2 (0.98, 0.20, -0.20, 0.98) * (mq - 0.4 * uAtmo.yz) + vec2 (0.4 * (mB - 0.5), 0.35 * (mA - 0.5) + 1.6 * (mC - 0.5));
    float mS = texture2D (uNoise, vec2 (sq.x * 0.5, sq.y * 1.35) + vec2 (0.41, 0.83)).r;
    mS = mix (0.5, mS, 0.3 + 0.7 * smoothstep (0.3, 0.68, mB));
    float mN = clamp ((0.5 * mA + 0.3 * mB + 0.2 * mS - 0.5) * 3.0, -1.0, 1.0);
    return vec3 (mN, mS, smoothstep (0.2, 0.9, mB));
}

// The mist's value before the creature's shadow: high-key, lighter towards the top, a little lighter
// where the cloud under it is dense and lit. y: view height (-0.5..0.5); sun 0..1.2. early (0..1):
// before the body is big and dense enough to shadow much of the mist, its own darker masses go
// deeper (the lightest values stay where they are), so the view keeps the same spread of values
// from the first wisps to the full creature
float mistValue (vec3 m, float y, float cloud, float sun, float early)
{
    float v = 0.785 + 0.13 * m.x + 0.12 * min (m.x, 0.0) + 0.30 * smoothstep (-0.15, 0.8, m.x) + 0.10 * smoothstep (0.5, 0.78, m.y)
            + 0.10 * (cloud - 0.5) + 0.14 * y + 0.05 * (sun - 0.6);
    float dk = max (0.95 - v, 0.0);
    return v - early * (0.008 + 0.18 * smoothstep (0.0, 0.45, dk));
}

// How early the stage is, for the mist (1 until the body is large, 0 once it is)
float mistEarly() { return 1.0 - smoothstep (0.60, 0.86, clamp (uSeedB.w, 0.0, 1.0)); }

// Asleep, the creature is a low, darker hump under the mist: a soft dome rising from the bottom of
// the view behind where it lies (a value shape with no edge; the field it leaves while it sleeps is
// too faint and too ragged to cast it). As a share of the body's shadow in the mist.
float sleepHump (vec2 p)
{
    float r = length (vec2 (p.x / 0.62, (p.y + 0.52) / 0.60));
    return uSleep * 0.55 * (1.0 - smoothstep (0.60, 1.0, r));
}

// The late stages' lift: the large dense body shadows much of the mist, so the rest is a step lighter.
// Not while asleep: the sleeping body's shadow is lifted (shadowM x (1 - 0.45 uSleep)), so the lift
// would only brighten the scene past the awake stages.
float mistLate() { return 0.045 * smoothstep (0.60, 0.86, clamp (uSeedB.w, 0.0, 1.0)) * (1.0 - clamp (uSleep, 0.0, 1.0)); }

// About one 8-bit step of triangular dither (interleaved gradient plus a hash): the pale, smooth mist
// bands into contours on an 8-bit target without it
vec3 outDither (vec2 frag)
{
    float ign = fract (52.9829189 * fract (dot (frag, vec2 (0.06711056, 0.00583715))));
    return vec3 ((ign + hash12 (frag + 7.7) - 1.0) / 255.0);
}
)GLSL";

const char* const c_noise = R"GLSL(
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
)GLSL";

const char* const c_helpers = R"GLSL(
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

// RING's muzzle over the mouth: bands of dark metal in an oval round the lips, its outline feathered
// into the skin (never a box: a straight-sided plate reads as a tile pasted over the face through
// the mist). h is mirrored (x >= 0); body is the ring's body mix; feather in head units.
// x: coverage (0..1), y: the band coordinate (0 at the top, 1 at the bottom, curving with the
// outline), z: the distance to the outline (outside > 0).
vec3 ringMuzzle (vec2 h, float mw, float mY, float open, float body, float feather)
{
    float top = mY + 0.032 + 0.22 * open;
    float bot = mY - 0.045 - open - 0.01 * body;
    float cy = 0.5 * (top + bot);
    vec2 r = vec2 (mw * (1.12 + 0.08 * body), 0.5 * (top - bot));
    float dz = sdEllipse (h - vec2 (0.0, cy), r);
    float cov = 1.0 - smoothstep (-feather, 0.3 * feather, dz);
    float halfH = r.y * sqrt (max (1.0 - (h.x * h.x) / (r.x * r.x), 0.04));
    float sv = clamp (0.5 - 0.5 * (h.y - cy) / halfH, 0.0, 1.0);
    return vec3 (cov, sv, dz);
}

// A rivet along the muzzle's upper band (i = 0, 1, 2 from the middle out): 0..1, inside the outline.
float ringRivet (vec2 h, float mw, float mY, float open, float body, float i)
{
    float top = mY + 0.032 + 0.22 * open;
    float bot = mY - 0.045 - open - 0.01 * body;
    vec2 r = vec2 (mw * (1.12 + 0.08 * body), 0.5 * (top - bot));
    float rx = r.x * (0.15 + 0.3 * i);
    float ry = 0.5 * (top + bot) + r.y * sqrt (max (1.0 - (rx * rx) / (r.x * r.x), 0.0)) - 0.012;
    return 1.0 - smoothstep (0.0035, 0.0065, length (h - vec2 (rx, ry)));
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
)GLSL";

const char* const c_head = R"GLSL(
    // ---- mirror: the blot is the same on both halves
    vec2 q = vec2 (abs (p.x), p.y);

    float breath = uHead.w;
    float scale = 1.0 + clamp (-uPitch / 12.0, -1.0, 2.0) * 0.065 + 0.014 * breath * (0.35 + uSleep)
                + uJaw * 0.025 * awake + uHead.y;
    vec2 h = (q - vec2 (0.0, 0.025 + uHead.x)) / scale;
    // the wings and tendrils follow the head a moment late
    vec2 hw = h + vec2 (0.0, (uHead.x - uHead.z) / scale);
    float aa = 1.5 * px / scale;

    // liquid: the whole skin wobbles
    vec2 wob = vec2 (sin (h.y * 17.0 + t * 1.9 + lph), sin (h.x * 13.0 + h.y * 6.0 + t * 1.4 - lph * 0.5));
    h += wob * lq * 0.006;
    hw += wob * lq * 0.006;

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
    // the jaw spring's opening: mid levels read as parted, only a roar gapes (the growl never reaches the chin)
    float jawOpen = pow (clamp (uJaw, 0.0, 1.05), 1.6);
    float open = max ((0.006 + jawOpen * (0.12 + 0.05 * sizeJ)) * awake, 0.003);
    open = max (open * (1.0 - 0.85 * uGateClosed), 0.003);   // the gate shut: the lips close
    // (a fast close squashes the chin a little)
    float chinDrop = open * 0.85 * (1.0 + 0.06 * clamp (-uJawVel, 0.0, 2.0));
    float chinY = -0.29 - chinDrop - 0.03 * jawM;
)GLSL";

const char* const c_body = R"GLSL(
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

    float dHead = smin (dCran, dCheek, 0.07 * SMK);
    dHead = smin (dHead, dJaw, 0.05 * SMK);
    dHead = smin (dHead, dChin, 0.06 * SMK);
    dHead += erH;
    dHead = smin (dHead, dHorn, 0.04 * SMK);
    dHead = smin (dHead, dSpike, 0.025 * SMK);

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
        // (never thinner than the smoke's half-resolution field can carry: a hairline feeler breaks into
        // a dotted, crisp straight line under the mist)
        dAnt = min (sdTaper (h, a0, a1, 0.013, 0.010), sdTaper (h, a1, a2, 0.010, 0.008));
        dAnt = min (dAnt, sdEllipse (rot (-0.5) * (h - a2), vec2 (0.026, 0.012) * an));
        dHead = smin (dHead, dAnt + erX, 0.015);
    }

    // the blot's wings spread sideways, swelling a little with the output
    float swell = 1.0 + 0.06 * uLevel * awake + 0.02 * breath;
    float span = uEvoWing.x;
    vec2 wq = vec2 (0.30, 0.0) + rot (-uEvoWing.y) * (hw - vec2 (0.30, 0.0));
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
            dSpur = min (dSpur, sdTaper (wq, wr, wr + (tip - wr) * wI, 0.020 * wI, 0.008) + (1.0 - smoothstep (0.0, 0.3, wI)) * 0.03);
        }
        // (kept inside the membrane, and never finer than the smoke's edge can carry: a spur running out
        // past the wing is a thin, straight, hard line in the sky)
        dSpur = max (dSpur, env + 0.015);
        dWing = max (dWing, -(length ((wq - vec2 (1.30, -0.19)) * vec2 (1.0, 1.3)) - 0.13 * fg));
        dWing = max (dWing, -(length ((wq - vec2 (1.26, 0.27)) * vec2 (1.0, 1.4)) - 0.10 * fg));
        dWing = smin (dWing, dSpur, 0.02);
    }

    // wing eyespots: pale holes with a dark pupil, a second, larger face in the wings
    // (they come with the wings and go, faster than the ink round them, while the ink flows into the
    // logo: a hole must never outlive its wing as a pale disc; carved with a soft shoulder, so the
    // hole's edge is a gradient in the smoke, never a hard pale rim)
    float eggGone = 1.0 - smoothstep (0.0, 0.35, egg);
    if (uEvoWing.z > 0.001)
    {
        float es = sqrt (uEvoWing.z) * Fw * eggGone;
        float re = length ((wq - vec2 (0.86, 0.03)) / vec2 (0.11, 0.08));
        // (born at radius 0 and a little below the surface, so nothing appears as a dot)
        float born = (1.0 - smoothstep (0.0, 0.25, es)) * 0.02;
        dWing = -smin (-dWing, (re - es) * 0.08 + born, 0.03);
        dWing = smin (dWing, (re - 0.38 * es) * 0.08 + 3.0 * born, 0.015);
    }

    float sat = (0.4 + uEvoMisc.w) * eggGone;
    float satOff = (1.0 - eggGone) * 0.04;
    dWing = min (dWing, length (wq - vec2 (1.32, -0.04)) - 0.035 * sat + satOff);
    dWing = min (dWing, length (wq - vec2 (1.44, 0.15)) - 0.022 * sat + satOff);
    dWing = min (dWing, length (wq - vec2 (0.96, 0.31)) - 0.030 * sat + satOff);
    dWing = min (dWing, length (wq - vec2 (1.24, -0.26)) - 0.024 * sat + satOff);
    dWing = min (dWing, length (wq - vec2 (0.50, 0.36)) - 0.018 * sat + satOff);
    dWing = dWing * spanK + erW;

    float dInk = smin (dHead, dWing + 0.010, 0.12 * SMK);

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
        float u = clamp ((hw.x - x0) / max (tli, 1.0e-3), 0.0, 1.0);
        float yy = y0 + (0.045 * sin (hw.x * (11.0 + 3.0 * fi) - t * (0.9 + 0.3 * fi) + fi * 2.1)
                         + 0.02 * sin (hw.x * 27.0 + t * 1.3 + fi)) * u - 0.07 * u * u * (fi - 0.6 - (i > 2 ? 3.0 : 0.0))
                 - 0.16 * u * u * reachInk;   // ink arms hang heavier than the chorus' light filaments
        float thick = mix (0.006 + 0.008 * reachInk, 0.0008 + 0.002 * reachInk, u);
        // fades along its length, so it reads as ink thinning out rather than a drawn line
        float dd = max (abs (hw.y - yy) - thick, max (x0 - hw.x, hw.x - (x0 + tli)));
        // and grows out of the cheek instead of starting with a squared-off end
        dd += 0.010 * (1.0 - smoothstep (0.0, 0.06, hw.x - x0));
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
        // (and capped below where the baked field saturates, 0.125 of the box: a saturated field meeting
        // the box distance is a kink along the box's edge, a faint rectangle drawn in the smoke)
        float dl = min (min (dLogoG, dLogoC), min (0.12, 0.10 * uLogoBox.z));
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
    // (only near the edge: inside, the per-quad gradient would show as steps in the shading; and not on
    // a crease, where the gradient folds over inside a pixel quad (a thin spine's axis, the line between
    // two parts): there it reads near zero, and the rescale stamps a dotted line along the crease)
    float grad = length (vec2 (dFdx (dInk), dFdy (dInk))) * scale / px;
    float dEdge = dInk / mix (1.0, clamp (grad, 0.25, 1.5), smoothstep (-0.04, -0.01, dInk) * smoothstep (0.3, 0.7, grad));
    float bleed = aa + 0.002 * edgeN;
)GLSL";

const char* const c_face = R"GLSL(

    // ---- the face's geometry: brow, cheekbones, muzzle, eyes and maw (no shading here)
    float browW = uEvoHead.w * Fm;
    float dBrow = sdTaper (h, vec2 (0.03, 0.118), vec2 (0.25, 0.205), 0.018, 0.030);
    float dBrow2 = sdTaper (h, vec2 (0.06, 0.185), vec2 (0.21, 0.265), 0.012, 0.020);
    float dCheekBone = sdTaper (h, vec2 (0.15, -0.005), vec2 (0.28, 0.035), 0.016, 0.010);
    float dNose = sdTaper (h, vec2 (0.0, 0.12), vec2 (0.0, -0.01 * (1.0 + snout)), 0.022, 0.032 + 0.012 * snout);
    float dMuzzle = sdEllipse (h - vec2 (0.0, -0.035), vec2 (0.08 + 0.05 * snout, 0.06 + 0.03 * snout));
    float dNostril = sdEllipse (rot (0.5) * (h - vec2 (0.034 + 0.01 * snout, -0.012 - 0.01 * snout)), vec2 (0.014, 0.006) * (1.0 + 0.4 * snout));

    // eyes: slanted almonds under the brows
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

    // more eyes open on the stranger forms: a crease first, then a slit of glow, then the lid opens
    vec3 eyB = vec3 (0.0);
    vec3 eyK = vec3 (0.0);
    vec3 eyC = vec3 (0.0);
    vec3 openX = clamp (uEvoEyes.xyz, 0.0, 1.0);
    float aaE = aa + 0.006 * sigma;
    vec2 cB = vec2 (0.075 * uCloud.y, 0.205 * cran);
    vec2 cK = vec2 (0.215 * uCloud.y, 0.005);
    vec2 cC = vec2 (0.0, 0.25 * cran);
    vec3 crease = smoothstep (0.0, 0.15, openX);
    if (openX.x + openX.y + openX.z > 0.001)
    {
        vec3 lid = smoothstep (0.4, 1.0, openX) * eyeOpen;
        eyB = almond (h, cB, -0.25, 0.030, 0.016 * lid.x + 0.001, aaE);
        eyK = almond (h, cK, -0.55, 0.026, 0.012 * lid.y + 0.001, aaE);
        eyC = almond (h, cC, 1.5708, 0.034, 0.014 * lid.z + 0.001, aaE);
    }

    // maw
    // a ragged slot, not a crescent: the corners turn down, the upper lip lifts off the teeth as the
    // jaw drops, and the lip edge is the cloud's own (unmirrored, so the two sides differ a little)
    float mx = h.x / mw;
    float mx2 = mx * mx;
    float lipCurve = sqrt (max (1.0 - mx2, 0.0));
    float rag = (C - 0.5) * 0.014 * (0.5 + 0.5 * sigma) + (vnoise (vec2 (h.x * 34.0, 3.0)) - 0.5) * 0.008;
    float rag2 = (vnoise (vec2 (h.x * 29.0, 8.0)) - 0.5) * 0.010 - (C - 0.5) * 0.010 * sigma;
    // the lips carry the syllables and the growl's shiver (capped well below the opening, never the chin)
    float lipMove = uLip * 0.012 * Fm * awake;
    float lipShiver = (0.0035 * uGrowl * uGrowlMod * sin (t * 44.0) + 0.002 * clamp (uRoarHold / 0.45, 0.0, 1.0) * sin (t * 47.0)) * awake * Fm;
    float yU = mY + 0.012 * (1.0 - mx2) - 0.020 * mx2 + open * (0.22 * lipCurve - 0.25 * mx2) + rag * (0.4 + 0.6 * lipCurve)
             + (0.5 * lipMove + lipShiver) * lipCurve;
    float yL = mY - 0.020 * mx2 - 0.002 * (1.0 - mx2) - open * (0.78 * lipCurve + 0.25 * mx2) + rag2 * (0.3 + 0.7 * lipCurve)
             - (0.5 * lipMove + lipShiver) * lipCurve;
    float mouthMid = mY - open * 0.28;
    float dMouth = max (h.x - mw, max (h.y - yU, yL - h.y));
    float dLips = sdEllipse ((h - vec2 (0.0, mouthMid)) / vec2 (1.0, 1.0 + open * 6.0), vec2 (mw * 1.08, 0.03));
)GLSL";

const char* const c_simCommon = R"GLSL(
varying vec2 vUv;

uniform vec2 uSimTexel;   // one active sim texel, in domain uv
uniform vec2 uSimRes;     // the active sim size, texels
uniform vec4 uSimS;       // sim textures: active / allocated (xy), the last texel centre (zw)
uniform float uAspect;    // width / height
uniform float uDt;        // the sim step, seconds

vec2 simUv (vec2 uv) { return min (uv * uSimS.xy, uSimS.zw); }
)GLSL";

const char* const c_cloudTarget = R"GLSL(

uniform vec4 uCloud;      // inkiness, eye spacing, crown spread, bloom phase
uniform vec4 uSeed;       // blot noise offset xy, cloud offset xy
uniform vec4 uDrift;      // the targets' slow drift: back xy, front xy

// The cloud the sky relaxes to (today's domain-warped field, three octaves), as a density.
float cloudTarget (vec2 p)
{
    // (rotated off the noise lattice, so no blocks line up with the screen)
    vec2 cw = mat2 (0.8, 0.6, -0.6, 0.8) * p * 1.55 + uSeed.zw + uDrift.xy;
    vec3 w1 = vnoised (cw * 0.9);
    vec3 w2 = vnoised (cw * 0.9 + vec2 (7.3, 2.9));
    vec2 cq = cw + 1.4 * vec2 (w1.x, w2.x) + 0.12 * uCloud.x * vec2 (w1.z, -w1.y);
    float c = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; i++)
    {
        c += amp * vnoise (cq);
        cq = mat2 (1.6, 1.2, -1.2, 1.6) * cq + vec2 (3.1, 1.7);
        amp *= 0.5;
    }
    c /= 0.9375;
    float theta = 0.49 - 0.05 * sin (uCloud.w + 6.2832 * w2.x);
    // billows with gaps between them, and a denser core in each (so the swirl has edges to wind up)
    float b = smoothstep (theta - 0.07, theta + 0.10, c);
    return b * (0.55 + 0.45 * smoothstep (theta + 0.05, theta + 0.25, c));
}

// The front fog's target: streaked, denser low (scaled by the fog opacity in the composite).
float fogTarget (vec2 p, float ct)
{
    vec2 cwF = p * 1.55 + uSeed.zw * 1.7 + uDrift.zw + vec2 (11.3, 4.7);
    float a = -0.18 * 0.8 + 0.12;
    float c = cos (a);
    float s = sin (a);
    vec2 r = mat2 (c, s, -s, c) * cwF;
    float sf = vnoise (r * vec2 (1.2, 3.6));
    float low = smoothstep (0.05, -0.45, p.y);
    return clamp ((sf + 0.3 * ct) / 1.3 * (0.55 + 0.9 * low), 0.0, 1.5);
}
)GLSL";

const char* const c_emitter = R"GLSL(

uniform sampler2D uCF;
uniform vec4 uCFS;
uniform sampler2D uE;
uniform float uFirst;
uniform float uFormK;     // how formed the body is (its motion pushes the fluid only once formed)

vec4 cfAt (vec2 uv) { return texture2D (uCF, min (uv * uCFS.xy, uCFS.zw)); }

// The creature at sim resolution: distance, density target, and the obstacle's own velocity
// (from how the distance changed since the last step, so every moving part pushes the fluid).
void main()
{
    vec2 uv = vUv;
    vec4 c = cfAt (uv);
    vec2 ex = vec2 (uSimTexel.x, 0.0);
    vec2 ey = vec2 (0.0, uSimTexel.y);
    float dR = cfAt (uv + ex).x;
    float dL = cfAt (uv - ex).x;
    float dT = cfAt (uv + ey).x;
    float dB = cfAt (uv - ey).x;
    vec2 g = vec2 (dR - dL, dT - dB);
    vec2 n = g / max (length (g), 1.0e-5);
    // thin parts still register: the densest of a few taps inside this texel
    vec2 hx = 0.3 * ex;
    vec2 hy = 0.3 * ey;
    float tgt = max (max (c.y, cfAt (uv + hx + hy).y), max (cfAt (uv - hx - hy).y, max (cfAt (uv + hx - hy).y, cfAt (uv - hx + hy).y)));
    float dPrev = texture2D (uE, simUv (uv)).x;
    float vn = uFirst > 0.5 ? 0.0 : clamp (-(c.x - dPrev) / max (uDt, 1.0e-3), -0.4, 0.4) * uSimRes.y;
    vn *= 1.0 - smoothstep (0.02, 0.08, c.x);   // the surface and the body move; the field far off only shimmers
    // only a formed body pushes: while it is still cloud its edge moves with the erosion, which reads
    // the medium, and pushing the medium with that would feed back into a bubble
    vn *= uFormK;
    gl_FragColor = vec4 (c.x, tgt, vn * n);
}
)GLSL";

const char* const c_advect = R"GLSL(

uniform sampler2D uV;
uniform float uDiss;

void main()
{
    vec2 v = texture2D (uV, simUv (vUv)).xy;
    vec2 back = vUv - v * uDt * uSimTexel;
    // (zw: a slow copy of the velocity, advected along, for the composite's moving detail)
    vec4 b = texture2D (uV, simUv (back));
    gl_FragColor = vec4 (b.xy * uDiss, b.zw);
}
)GLSL";

const char* const c_curl = R"GLSL(

uniform sampler2D uV;

void main()
{
    vec2 ex = vec2 (uSimTexel.x, 0.0);
    vec2 ey = vec2 (0.0, uSimTexel.y);
    float w = (texture2D (uV, simUv (vUv + ex)).y - texture2D (uV, simUv (vUv - ex)).y)
            - (texture2D (uV, simUv (vUv + ey)).x - texture2D (uV, simUv (vUv - ey)).x);
    gl_FragColor = vec4 (0.5 * w, 0.0, 0.0, 1.0);
}
)GLSL";

const char* const c_forces = R"GLSL(

uniform sampler2D uV;
uniform sampler2D uWc;
uniform sampler2D uE;
uniform sampler2D uDye;
uniform vec4 uDyeS;
uniform vec4 uAmb;        // the weather's stream offsets: octave 1 xy, octave 2 xy
uniform vec4 uSimK;       // stream amplitude, octave 2 weight, confinement, relax rate (1/s)
uniform vec4 uBuoy;       // rise per heat, sink per excess density, edge shear amplitude, shedding outflow
uniform vec4 uGather;     // formation, -, spiral strength (texels/s, signed), face centre y
uniform vec4 uSplat[8];   // view x, y, radius (view heights), force (texels/s^2)
uniform vec4 uSplatDir[8];// direction xy, heat rate, dye rate (+ fog, - ink)
uniform vec4 uVort[8];    // view x, y, core radius, signed peak speed (view heights/s)
uniform vec4 uFrill;      // ridge centre y (view), root radius, length scale, amount
uniform float uBands[32];
uniform vec4 uEns;        // ensemble width, tendril y (view), -, -
uniform float uSeedMode;

float eAt (vec2 uv) { return texture2D (uE, simUv (uv)).x; }

// The weather's stream function: two octaves of drifting noise, steered round the head (the
// formed body is still), plus a shear along the silhouette, odd in x so both sides peel alike.
float psiAt (vec2 uv)
{
    // (in view heights, in units of view heights squared per second: the same weather on every tier)
    vec2 xt = (uv - 0.5) * vec2 (uAspect, 1.0) * 78.0;
    float ps = uSimK.x * (vnoise (xt / 90.0 + uAmb.xy) + uSimK.y * vnoise (xt / 36.0 + uAmb.zw));
    float d = eAt (uv);
    ps *= mix (1.0, smoothstep (-0.02, 0.10, d), uGather.x);
    float x = (uv.x - 0.5) * uAspect;
    // (a gentle sign change on the axis: a sharp one would shoot a jet up and down the fold)
    ps += x / (abs (x) + 0.2) * uBuoy.z * exp (-(d / 0.06) * (d / 0.06));
    return ps;
}

// Gaussian-core vortices (peak speed at the core radius).
vec2 vortexAt (vec2 p)
{
    vec2 v = vec2 (0.0);
    for (int i = 0; i < 8; i++)
    {
        vec4 vt = uVort[i];
        vec2 r = p - vt.xy;
        float rc = max (vt.z, 1.0e-3);
        v += vt.w / rc * exp (0.5 * (1.0 - dot (r, r) / (rc * rc))) * vec2 (-r.y, r.x);
    }
    return v * uSimRes.y;
}

void main()
{
    vec2 uv = vUv;
    vec2 p = (uv - 0.5) * vec2 (uAspect, 1.0);
    vec2 ex = vec2 (uSimTexel.x, 0.0);
    vec2 ey = vec2 (0.0, uSimTexel.y);
    vec4 V0 = texture2D (uV, simUv (uv));
    vec2 v = V0.xy;
    vec4 E = texture2D (uE, simUv (uv));
    float d = E.x;

    // the target flow: the weather, the vortices, the chorus stretch, the formation spiral
    // (the stream function is in view units: one texel is 1 / rows of a view height, and the
    // velocity is wanted in texels per second, hence rows squared)
    vec2 tgt = 0.5 * vec2 (psiAt (uv + ey) - psiAt (uv - ey), -(psiAt (uv + ex) - psiAt (uv - ex))) * uSimRes.y * uSimRes.y;
    tgt += vortexAt (p);
    tgt.x += 6.0 * uEns.x * (p.x / (abs (p.x) + 0.03)) * exp (-(p.y - uEns.y) * (p.y - uEns.y) / 0.02);
    vec2 rf = p - vec2 (0.0, uGather.w);
    float rl = max (length (rf), 1.0e-3);
    vec2 rh = rf / rl;
    vec2 th = vec2 (-rh.y, rh.x) * (p.x >= 0.0 ? 1.0 : -1.0);
    tgt += (-0.4 * rh + 0.3 * th) * uGather.z * exp (-rl / 0.35);

    if (uSeedMode > 0.5)
    {
        gl_FragColor = vec4 (tgt, tgt);
        return;
    }

    v += (tgt - v) * (1.0 - exp (-uSimK.w * uDt));

    // vorticity confinement: spin the curls back up
    float wC = texture2D (uWc, simUv (uv)).x;
    vec2 gw = 0.5 * vec2 (abs (texture2D (uWc, simUv (uv + ex)).x) - abs (texture2D (uWc, simUv (uv - ex)).x),
                          abs (texture2D (uWc, simUv (uv + ey)).x) - abs (texture2D (uWc, simUv (uv - ey)).x));
    vec2 N = gw / (length (gw) + 1.0e-5);
    v += uDt * uSimK.z * vec2 (N.y, -N.x) * wC;

    // heat rises, dense ink sinks a little
    vec4 D = texture2D (uDye, min (uv * uDyeS.xy, uDyeS.zw));
    v.y += uDt * (uBuoy.x * max (D.b, 0.0) - uBuoy.y * max (D.r - 0.6, 0.0));

    // the audio's pushes
    for (int i = 0; i < 8; i++)
    {
        vec4 s = uSplat[i];
        vec2 r = (p - s.xy) / max (s.z, 1.0e-3);
        v += uDt * s.w * exp (-dot (r, r)) * uSplatDir[i].xy;
    }
    // the vocoder: small upward puffs at the frill's ridge tips (even bands on the left, odd on the right)
    if (uFrill.w > 0.001)
    {
        for (int k = 0; k < 32; k++)
        {
            float fk = float (k);
            float side = mod (fk, 2.0) > 0.5 ? 1.0 : -1.0;
            float a = (floor (fk * 0.5) + 0.5) * 1.75 / 16.0;
            float band = clamp (uBands[k], 0.0, 1.0);
            float rr = uFrill.y + uFrill.z * (0.035 + 0.17 * band);
            vec2 r = (p - vec2 (side * sin (a) * rr, uFrill.x - cos (a) * rr)) / 0.04;
            v.y += uDt * 20.0 * band * uFrill.w * exp (-dot (r, r));
        }
    }

    // the creature as a moving obstacle: still where formed, except for its own motion
    float F = uGather.x;
    v = mix (v, E.zw, 0.7 * F * smoothstep (0.0, -0.02, d));
    vec2 gE = vec2 (eAt (uv + ex) - eAt (uv - ex), eAt (uv + ey) - eAt (uv - ey));
    vec2 nE = gE / max (length (gE), 1.0e-6);
    float surf = exp (-(d / 0.015) * (d / 0.015)) * F;
    vec2 rel = v - E.zw;
    v -= nE * min (dot (rel, nE), 0.0) * surf * 0.8;   // free slip: nothing flows into the surface
    // the surface sheds into the cloud
    v += nE * uBuoy.w * step (0.0, d) * (1.0 - smoothstep (0.0, 0.03, d)) * uDt * 3.0;

    // a sponge at the border, towards the weather
    vec2 edgeT = min (uv, 1.0 - uv) / uSimTexel;
    v = mix (v, tgt, 0.5 * (1.0 - smoothstep (0.0, 8.0, min (edgeT.x, edgeT.y))));

    if (! (abs (v.x) + abs (v.y) < 1.0e5))
        v = vec2 (0.0);
    float sp = length (v);
    if (sp > 60.0)
        v *= 60.0 / sp;
    gl_FragColor = vec4 (v, V0.zw);
}
)GLSL";

const char* const c_divergence = R"GLSL(

uniform sampler2D uV;
uniform vec4 uDiv[3];     // divergence sources (the sub's rings, the temples): view x, y, radius, strength
uniform float uDivK;      // the time this solve stands for, in 1/30 s steps (same push at any step rate)

void main()
{
    vec2 ex = vec2 (uSimTexel.x, 0.0);
    vec2 ey = vec2 (0.0, uSimTexel.y);
    float div = 0.5 * (texture2D (uV, simUv (vUv + ex)).x - texture2D (uV, simUv (vUv - ex)).x
                     + texture2D (uV, simUv (vUv + ey)).y - texture2D (uV, simUv (vUv - ey)).y);
    vec2 p = (vUv - 0.5) * vec2 (uAspect, 1.0);
    for (int i = 0; i < 3; i++)
    {
        vec2 r = (p - uDiv[i].xy) / max (uDiv[i].z, 1.0e-3);
        div -= uDiv[i].w * uDivK * exp (-dot (r, r));
    }
    gl_FragColor = vec4 (div, 0.0, 0.0, 1.0);
}
)GLSL";

const char* const c_jacobi = R"GLSL(

uniform sampler2D uP;
uniform sampler2D uDv;
uniform float uWarm;      // the first iteration starts from a fraction of the last frame's pressure

float pAt (vec2 uv)
{
    // pressure 0 beyond the border
    return (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) ? 0.0 : texture2D (uP, simUv (uv)).x;
}

void main()
{
    vec2 ex = vec2 (uSimTexel.x, 0.0);
    vec2 ey = vec2 (0.0, uSimTexel.y);
    float s = pAt (vUv + ex) + pAt (vUv - ex) + pAt (vUv + ey) + pAt (vUv - ey);
    gl_FragColor = vec4 ((s * uWarm - texture2D (uDv, simUv (vUv)).x) * 0.25, 0.0, 0.0, 1.0);
}
)GLSL";

const char* const c_gradient = R"GLSL(

uniform sampler2D uV;
uniform sampler2D uP;

float pAt (vec2 uv)
{
    return (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) ? 0.0 : texture2D (uP, simUv (uv)).x;
}

void main()
{
    vec2 ex = vec2 (uSimTexel.x, 0.0);
    vec2 ey = vec2 (0.0, uSimTexel.y);
    vec4 V0 = texture2D (uV, simUv (vUv));
    vec2 v = V0.xy - 0.5 * vec2 (pAt (vUv + ex) - pAt (vUv - ex), pAt (vUv + ey) - pAt (vUv - ey));
    if (! (abs (v.x) + abs (v.y) < 1.0e5))
        v = vec2 (0.0);
    float sp = length (v);
    if (sp > 60.0)
        v *= 60.0 / sp;
    vec2 slow = V0.zw + (v - V0.zw) * (1.0 - exp (-uDt / 0.5));
    if (! (abs (slow.x) + abs (slow.y) < 1.0e5))
        slow = v;
    gl_FragColor = vec4 (v, slow);
}
)GLSL";

const char* const c_dye = R"GLSL(

uniform sampler2D uDye;
uniform sampler2D uV;
uniform sampler2D uE;
uniform sampler2D uCF;
uniform vec4 uDyeS;
uniform vec4 uCFS;
uniform vec2 uDyeTexel;
uniform vec2 uDyeRes;
uniform vec4 uGatherD;    // gather speed (texels/s, signed), formation, -, error-corrected (1) or plain (0)
uniform vec4 uInk;        // solidity, bleed, shedding band (view), red ink decay (s)
uniform vec4 uFogD;       // -, eye thinning, throat thinning, -
uniform vec4 uEyeP;       // main eye centre (view, x > 0), throat y, throat radius
uniform vec4 uSplat[8];
uniform vec4 uSplatDir[8];
uniform vec4 uFrill;
uniform float uBands[32];
uniform float uSeedMode;

vec4 dyeAt (vec2 uv) { return texture2D (uDye, min (uv * uDyeS.xy, uDyeS.zw)); }
vec2 velAt (vec2 uv) { return texture2D (uV, simUv (uv)).xy * uSimTexel; }   // domain uv per second

void main()
{
    vec2 uv = vUv;
    vec2 p = (uv - 0.5) * vec2 (uAspect, 1.0);
    float ct = cloudTarget (p);
    float ft = fogTarget (p, ct);
    if (uSeedMode > 0.5)
    {
        gl_FragColor = vec4 (ct, ft, 0.0, 0.0);
        return;
    }
    float dt = uDt;
    float F = uGatherD.y;

    // the creature at this point (sim resolution) and the pull that gathers the cloud onto it
    vec4 E = texture2D (uE, simUv (uv));
    float d = E.x;
    vec2 ex = vec2 (uSimTexel.x, 0.0);
    vec2 ey = vec2 (0.0, uSimTexel.y);
    vec2 gd = vec2 (texture2D (uE, simUv (uv + ex)).x - texture2D (uE, simUv (uv - ex)).x,
                    texture2D (uE, simUv (uv + ey)).x - texture2D (uE, simUv (uv - ey)).x);
    // (only where the distance is a real distance: far off it is noise, and a unit vector of noise
    // would gather the sky into cells)
    vec2 nd = gd / max (length (gd), 2.0 * uSimTexel.y);
    vec2 vg = -uGatherD.x * nd * (1.0 - smoothstep (0.0, 0.10, d)) * smoothstep (-0.04, 0.0, d) * uSimTexel;

    // advection: back, forward and back again, corrected and limited to the texels round the backtrace
    vec2 v0 = velAt (uv) + vg;
    vec2 back = uv - v0 * dt;
    vec4 adv = dyeAt (back);
    if (uGatherD.w > 0.5)
    {
        vec2 fwd = uv + v0 * dt;
        vec4 phiT = dyeAt (fwd - (velAt (fwd) + vg) * dt);
        adv += 0.5 * (dyeAt (uv) - phiT);
        vec2 b0 = (floor (back * uDyeRes - 0.5) + 0.5) * uDyeTexel;
        vec4 t00 = dyeAt (b0);
        vec4 t10 = dyeAt (b0 + vec2 (uDyeTexel.x, 0.0));
        vec4 t01 = dyeAt (b0 + vec2 (0.0, uDyeTexel.y));
        vec4 t11 = dyeAt (b0 + uDyeTexel);
        adv = clamp (adv, min (min (t00, t10), min (t01, t11)), max (max (t00, t10), max (t01, t11)));
    }
    // where the gather converges on the edge it steepens the cloud into a front, which the limiter
    // keeps sharp: a little diffusion there (a terraced rim round every blot and hole otherwise)
    float gz = (1.0 - smoothstep (0.0, 0.10, d)) * smoothstep (-0.06, 0.0, d) * clamp (abs (uGatherD.x) * 0.1, 0.0, 1.0);
    if (gz > 0.001)
    {
        vec4 nb = 0.25 * (dyeAt (back + vec2 (uDyeTexel.x, 0.0)) + dyeAt (back - vec2 (uDyeTexel.x, 0.0))
                        + dyeAt (back + vec2 (0.0, uDyeTexel.y)) + dyeAt (back - vec2 (0.0, uDyeTexel.y)));
        adv.rba = mix (adv.rba, nb.rba, 0.35 * gz);
    }
    // the front fog runs faster than the back cloud (parallax)
    adv.g = dyeAt (uv - 1.6 * v0 * dt).g;

    vec4 cf = texture2D (uCF, min (uv * uCFS.xy, uCFS.zw));
    float rho = adv.r;

    // the sky relaxes to the procedural cloud: slowly in the open, fast at the domain edge (free
    // inflow), not at all inside a formed body
    vec2 edgeD = min (uv, 1.0 - uv) * uDyeRes;
    float edgeK = 1.0 - smoothstep (0.0, 4.0, min (edgeD.x, edgeD.y));
    float tauR = mix (mix (30.0, 16.0, smoothstep (0.0, 0.35, d)), 2.0, edgeK);
    float relax = (1.0 - exp (-dt / tauR)) * max (max (smoothstep (-0.02, 0.02, d), edgeK), 1.0 - F);
    rho += (ct - rho) * relax;

    // the creature condenses into the medium: held while formed, let go as it fades
    // (nothing while it is not there at all: the faint field left asleep would slowly fill the cloud
    // with a smooth, pale copy of itself)
    float kInj = 1.0 - exp (-dt * mix (0.05, 6.0, F * F) * smoothstep (0.04, 0.3, F));
    rho = mix (rho, max (rho, cf.y), kInj);
    // wisps outside it fade back into the cloud in about 5 s
    // (faded in across the edge: a rate that switches at the silhouette leaves a step in the cloud,
    // a hard terraced rim round every hole and blot)
    if (rho > ct)
        rho = mix (rho, ct + (rho - ct) * exp (-dt / 5.0), smoothstep (-0.01, 0.03, d));
    // the edge sheds its surface into the cloud (only a body that is there sheds: the faint field left
    // while it sleeps would otherwise ring itself with ink and leave a pale disc in the middle)
    // (a soft onset: a step at the edge rings every hole with a hard rim at the dye's resolution)
    float shed = smoothstep (-0.004, 0.008, d) * (1.0 - smoothstep (0.0, uInk.z, d)) * smoothstep (0.15, 0.5, F);
    rho += dt * (0.3 + 0.7 * F) * uInk.x * uInk.y * 0.6 * shed;
    // the formed core keeps its mirror image (the fluid round it does not)
    float occ = smoothstep (0.0, -0.02, d);
    rho = mix (rho, 0.5 * (rho + dyeAt (vec2 (1.0 - uv.x, uv.y)).r), 0.25 * occ * F);

    // the front fog: relaxes to its target, thinner over the eyes, and at the throat while a roar holds
    float fog = adv.g + (ft - adv.g) * (1.0 - exp (-dt / 6.0));
    vec2 qx = vec2 (abs (p.x), p.y);
    vec2 er = (qx - uEyeP.xy) / vec2 (0.10, 0.07);
    fog -= fog * clamp (uFogD.y * exp (-dot (er, er)) * dt * 1.5, 0.0, 1.0);
    vec2 tr = (p - vec2 (0.0, uEyeP.z)) / max (uEyeP.w, 0.02);
    fog -= fog * clamp (uFogD.z * exp (-dot (tr, tr)) * dt * 3.0, 0.0, 1.0);

    // heat (decays in a second; the emissive parts warm the medium) and the ink's pigment: held in
    // the formed body, carried off its surface by the swirl as dark smoke, fading in a few seconds
    float heatV = adv.b * exp (-dt) + dt * 0.3 * cf.z * F;
    float red = adv.a * exp (-dt / max (uInk.w, 0.5));
    float redT = cf.y * F * F * (0.4 + 0.6 * uInk.x) * occ;
    if (redT > red)
        red += (redT - red) * (1.0 - exp (-dt / 0.6));
    red += dt * 0.5 * (0.3 + 0.7 * F) * uInk.x * uInk.y * shed;

    for (int i = 0; i < 8; i++)
    {
        vec4 s = uSplat[i];
        vec2 r = (p - s.xy) / max (s.z, 1.0e-3);
        float g = exp (-dot (r, r)) * dt;
        heatV += uSplatDir[i].z * g;
        float dk = uSplatDir[i].w;
        fog += max (dk, 0.0) * g;
        rho += max (-dk, 0.0) * g;
    }
    if (uFrill.w > 0.001)
    {
        for (int k = 0; k < 32; k++)
        {
            float fk = float (k);
            float side = mod (fk, 2.0) > 0.5 ? 1.0 : -1.0;
            float a = (floor (fk * 0.5) + 0.5) * 1.75 / 16.0;
            float band = clamp (uBands[k], 0.0, 1.0);
            float rr = uFrill.y + uFrill.z * (0.035 + 0.17 * band);
            vec2 r = (p - vec2 (side * sin (a) * rr, uFrill.x - cos (a) * rr)) / 0.03;
            heatV += dt * 0.2 * band * uFrill.w * exp (-dot (r, r));
        }
    }

    vec4 o = vec4 (rho, fog, heatV, red);
    if (! (abs (o.x) + abs (o.y) + abs (o.z) + abs (o.w) < 1.0e5))
        o = vec4 (ct, ft, 0.0, 0.0);
    gl_FragColor = clamp (o, 0.0, 4.0);
}
)GLSL";

const char* const c_light = R"GLSL(

uniform sampler2D uDye;
uniform sampler2D uCF;
uniform vec4 uDyeS;
uniform vec4 uCFS;
uniform vec2 uDyeTexel;
uniform vec4 uForm;
uniform vec4 uLightDir;   // towards the light (view), phase, silver lining
uniform vec4 uSteps;      // sun steps, emitter taps, first step (view), step growth
uniform vec4 uEmit[4];    // eyes: view x (> 0, mirrored), y, radius, intensity (main, brow, cheek, centre)
uniform vec4 uThroat;     // the throat: view x, y, radius, intensity

float rhoAt (vec2 uv, float bodyK)
{
    vec4 D = texture2D (uDye, min (uv * uDyeS.xy, uDyeS.zw));
    float c = texture2D (uCF, min (uv * uCFS.xy, uCFS.zw)).y;
    return max (D.r, 0.0) + 0.6 * max (D.g, 0.0) + c * uForm.x * uForm.x * bodyK;
}

// An emitter seen through the ink: inverse-square-ish falloff, attenuated along the way.
float glowFrom (vec2 uv, vec2 p, vec2 ep, float r, float I)
{
    vec2 dv = ep - p;
    float dist = length (dv);
    vec2 duv = dv / vec2 (uAspect, 1.0);
    float tau = 0.0;
    for (int k = 0; k < 4; k++)
    {
        if (float (k) >= uSteps.y)
            break;
        tau += rhoAt (uv + duv * ((float (k) + 0.5) / uSteps.y), 1.0);
    }
    tau *= dist / uSteps.y;
    return I / (1.0 + dist * dist / (r * r)) * exp (-10.0 * tau);
}

void main()
{
    vec2 uv = vUv;
    vec2 p = (uv - 0.5) * vec2 (uAspect, 1.0);
    // (the body sits in front of the cloud: it shades itself, but casts no shadow on the cloud behind)
    float bodyK = smoothstep (0.0, 0.45, texture2D (uCF, min (uv * uCFS.xy, uCFS.zw)).y * uForm.x);
    float r0 = rhoAt (uv, bodyK);
    vec2 L = uLightDir.xy;
    vec2 Luv = L / vec2 (uAspect, 1.0);

    // the sun: a short march towards the light, geometric steps (no per-texel jitter: this pass is
    // magnified six times in the composite, where a jittered start shows as blocks)
    float jit = 0.5;
    float dl = uSteps.z;
    float s = jit * dl;
    float tau = 0.0;
    for (int i = 0; i < 6; i++)
    {
        if (float (i) >= uSteps.x)
            break;
        s += dl;
        tau += rhoAt (uv + Luv * s, bodyK) * dl;
        dl *= uSteps.w;
    }
    tau *= 9.0;
    float Ts = exp (-tau);
    float bp = mix (Ts, 2.0 * Ts * (1.0 - exp (-2.0 * tau)), 0.5);
    float ms = (exp (-tau) + 0.5 * exp (-0.4 * tau) + 0.25 * exp (-0.16 * tau)) / 1.75;
    vec2 ex = vec2 (uDyeTexel.x, 0.0);
    vec2 ey = vec2 (0.0, uDyeTexel.y);
    float rT = rhoAt (uv + ey, bodyK);
    float rB = rhoAt (uv - ey, bodyK);
    // the rim from a wide footprint (three texels each way): this pass is magnified six times, and a
    // narrow rim turns into a hard, jagged bright line round every edge
    float rT2 = rhoAt (uv + 3.0 * ey, bodyK), rB2 = rhoAt (uv - 3.0 * ey, bodyK);
    float rR2 = rhoAt (uv + 3.0 * ex, bodyK), rL2 = rhoAt (uv - 3.0 * ex, bodyK);
    vec2 g = 0.333 * vec2 (rR2 - rL2, rT2 - rB2);
    float Tv = exp (-3.0 * (0.4 * r0 + 0.15 * (rT2 + rB2 + rR2 + rL2)));
    // (weighted by the slope itself: where the smoke is nearly flat its direction is noise, and a
    // normalised noise flips the rim on and off from texel to texel)
    float gl0 = length (g);
    float facing = max (0.0, -dot (g / max (gl0, 1.0e-4), L)) * smoothstep (0.01, 0.08, gl0);
    float silver = 2.4 * Tv * (1.0 - Tv) * Ts * facing * facing * uLightDir.w;
    float sun = bp * uLightDir.z + 0.6 * ms + silver;

    // sky light from above, a little bounce from below
    float sky = exp (-1.5 * (rT + rT2)) + 0.3 * (1.0 - exp (-rhoAt (uv - 2.0 * ey, bodyK)));

    // the eyes and the throat, shining through the ink (red carries furthest, in the composite)
    float side = p.x >= 0.0 ? 1.0 : -1.0;
    float eg = 0.0;
    for (int i = 0; i < 4; i++)
    {
        vec4 em = uEmit[i];
        if (em.w > 0.0)
            eg += glowFrom (uv, p, vec2 (em.x * side, em.y), em.z, em.w);
    }
    float tg = uThroat.w > 0.0 ? glowFrom (uv, p, uThroat.xy, uThroat.z, uThroat.w) : 0.0;
    gl_FragColor = vec4 (sun, sky, eg, tg);
}
)GLSL";

const char* const c_bright = R"GLSL(
varying vec2 vUv;

uniform sampler2D uCF;
uniform sampler2D uLight;
uniform sampler2D uDye;
uniform vec4 uCFS;
uniform vec4 uLightS;
uniform vec4 uDyeS;
uniform vec3 uEyeCol;
uniform vec3 uThroatCol;
uniform vec3 uEmitCol;
uniform vec4 uBloomK;     // threshold, knee

void main()
{
    vec4 cf = texture2D (uCF, min (vUv * uCFS.xy, uCFS.zw));
    vec4 Lt = texture2D (uLight, min (vUv * uLightS.xy, uLightS.zw));
    vec4 D = texture2D (uDye, min (vUv * uDyeS.xy, uDyeS.zw));
    vec3 E = cf.z * uEmitCol * 0.6 + Lt.b * uEyeCol * 0.6 + Lt.a * uThroatCol * 0.5 + D.b * D.b * vec3 (1.0, 0.45, 0.18) * 1.5;
    // (rolled off: a roar widens the glow a little, it never turns the face into a lamp)
    E /= 1.0 + 0.6 * max (E.r, max (E.g, E.b));
    float br = max (E.r, max (E.g, E.b));
    float soft = clamp (br - uBloomK.x + uBloomK.y, 0.0, 2.0 * uBloomK.y);
    soft = soft * soft / (4.0 * uBloomK.y + 1.0e-4);
    gl_FragColor = vec4 (E * (max (soft, br - uBloomK.x) / max (br, 1.0e-4)), 1.0);
}
)GLSL";

const char* const c_down = R"GLSL(
varying vec2 vUv;

uniform sampler2D uSrc;
uniform vec4 uSrcS;
uniform vec2 uSrcTexel;

vec3 s (vec2 o) { return texture2D (uSrc, min ((vUv + o * uSrcTexel) * uSrcS.xy, uSrcS.zw)).rgb; }

// 13 taps, weighted as overlapping boxes (stable under motion)
void main()
{
    vec3 c = s (vec2 (0.0)) * 0.125;
    c += (s (vec2 (-2.0, 2.0)) + s (vec2 (2.0, 2.0)) + s (vec2 (-2.0, -2.0)) + s (vec2 (2.0, -2.0))) * 0.03125;
    c += (s (vec2 (0.0, 2.0)) + s (vec2 (-2.0, 0.0)) + s (vec2 (2.0, 0.0)) + s (vec2 (0.0, -2.0))) * 0.0625;
    c += (s (vec2 (-1.0, 1.0)) + s (vec2 (1.0, 1.0)) + s (vec2 (-1.0, -1.0)) + s (vec2 (1.0, -1.0))) * 0.125;
    gl_FragColor = vec4 (c, 1.0);
}
)GLSL";

const char* const c_up = R"GLSL(
varying vec2 vUv;

uniform sampler2D uSrc;
uniform vec4 uSrcS;
uniform vec2 uSrcTexel;
uniform float uUpGain;

vec3 s (vec2 o) { return texture2D (uSrc, min ((vUv + o * uSrcTexel) * uSrcS.xy, uSrcS.zw)).rgb; }

// a 9-tap tent, added onto the level above
void main()
{
    vec3 c = s (vec2 (0.0)) * 4.0;
    c += (s (vec2 (0.0, 1.0)) + s (vec2 (-1.0, 0.0)) + s (vec2 (1.0, 0.0)) + s (vec2 (0.0, -1.0))) * 2.0;
    c += s (vec2 (-1.0, 1.0)) + s (vec2 (1.0, 1.0)) + s (vec2 (-1.0, -1.0)) + s (vec2 (1.0, -1.0));
    gl_FragColor = vec4 (c * (uUpGain / 16.0), 1.0);
}
)GLSL";

const char* const c_copy = R"GLSL(
varying vec2 vUv;

uniform sampler2D uSrc;
uniform vec4 uSrcS;
uniform float uDither;   // 1: about one 8-bit step of triangular dither (an upscale onto the 8-bit target)

void main()
{
    vec4 c = texture2D (uSrc, min (vUv * uSrcS.xy, uSrcS.zw));
    if (uDither > 0.5)
    {
        vec2 fc = gl_FragCoord.xy;
        float a = fract (52.9829189 * fract (dot (fc, vec2 (0.06711056, 0.00583715))));
        vec3 p3 = fract (vec3 (fc.xyx) * 0.1031);
        p3 += dot (p3, p3.yzx + 33.33);
        float b = fract ((p3.x + p3.y) * p3.z);
        c.rgb += (a + b - 1.0) / 255.0;
    }
    gl_FragColor = c;
}
)GLSL";

const char* const c_probe = R"GLSL(
void main()
{
    gl_FragColor = vec4 (-2.5, 0.001, 1000.0, 0.5);
}
)GLSL";

const char* const c_cfPre = R"GLSL(
varying vec2 vUv;

uniform vec2 uOutRes;
uniform sampler2D uDye;
uniform vec4 uDyeS;
uniform float uDyeOn;
uniform vec4 uSpark;      // spark seed, strength
)GLSL";

const char* const c_cfMainPre = R"GLSL(

// The creature as a field: distance (view heights), density target (a pillow profile with soft
// holes), emission and red pigment. Exactly mirror-symmetric (the cloud it erodes along is read on
// both sides). The only place the distance field is evaluated in the fluid tiers.
void main()
{
    vec2 p = (vUv - 0.5) * vec2 (uAspect, 1.0);
    float px = 1.0 / uOutRes.y;

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

    // the cloud it condenses from: the last frame's medium, read on both sides
    float dA = texture2D (uDye, min (vUv * uDyeS.xy, uDyeS.zw)).r;
    float dB = texture2D (uDye, min (vec2 (1.0 - vUv.x, vUv.y) * uDyeS.xy, uDyeS.zw)).r;
    float cloud = mix (0.55, clamp (0.5 * (dA + dB), 0.0, 1.5), uDyeOn);
    float C = clamp (cloud, 0.1, 0.95);

)GLSL";

const char* const c_cfPost = R"GLSL(

    // ---- the medium's view of it
    float dV = dEdge * scale;
    // (its onset is smooth and starts just outside the silhouette: a pillow that rises steeply from
    // the edge steps at the dye's resolution, and every hole and small blot gets a hard terraced rim
    // in the cloud and in the light)
    // (and much wider while the ink flows into the logo and back: the moving front of the morph would
    // otherwise sweep across the wings as a hard-edged pale plateau)
    float morphW = sin (3.1416 * gl);
    float tP = clamp ((0.008 + 0.04 * morphW - dV) / (0.055 + 0.25 * morphW), 0.0, 1.0);
    tP = tP * tP * (3.0 - 2.0 * tP);
    float pillow = sqrt (tP * (2.0 - tP));
    float keep = 1.0 - gl;
    float wingK = smoothstep (0.30, 0.60, h.x);
    vec2 fk2 = (h - vec2 (0.0, -0.02)) / vec2 (0.26, 0.20);
    float Fpart = max (mix (Fh, Fw, wingK), Fe * exp (-dot (fk2, fk2)) * 0.85);
    Fpart = mix (Fpart, 1.0, gl);

    // soft holes: the eyes, the extra eyes, the maw, and the chest in overtone Replace
    vec3 bright = smoothstep (0.15, 0.4, openX) * Fe * keep;
    float holes = mix (1.0, smoothstep (-0.014, 0.002, dEye), Fe * keep);   // (inside the crisp eye: no dark rim round it)
    holes *= 1.0 - max (max (eyB.x * bright.x, eyK.x * bright.y), eyC.x * bright.z);
    holes *= mix (1.0, smoothstep (-0.014, 0.002, dMouth), Fm * keep);
    float ovA = uOvLevel * awake * Fh * keep;
    vec2 cc = vec2 (h.x / 0.15, (h.y + 0.28 + chinDrop + 0.03 * jawM) / 0.09);
    holes *= 1.0 - 0.8 * uOvReplace * ovA * exp (-dot (cc, cc));

    // broad ridges as extra density: they cast shadow and catch light, never draw lines
    float thick = (0.6 * smoothstep (0.03, -0.03, dBrow) + 0.3 * smoothstep (0.03, -0.03, dBrow2)) * browW
                + (0.35 * smoothstep (0.03, -0.03, dCheekBone) + 0.3 * smoothstep (0.03, -0.03, dNose)) * Fm
                + 0.5 * snout * smoothstep (0.04, -0.04, dMuzzle) * Fh
                + 0.4 * uEvoWing.w * smoothstep (0.03, -0.03, dSpur) * Fx
                + 0.35 * smoothstep (0.03, -0.03, dHorn) * Fx
                + 0.4 * smoothstep (0.03, -0.03, dFig)
                - 0.3 * smoothstep (0.03, -0.03, dSocket) * Fm
                - 0.3 * smoothstep (0.012, -0.006, dNostril) * Fm;
    float solidK = 0.55 + 0.45 * uEvoInk.x;
    float rho = Fpart * pillow * holes * solidK * (1.0 + 0.35 * thick);

    // ---- emission (scalar; the composite tints it by region)
    float glowK = uEvoPal.z;
    float eI = ((0.75 + 1.7 * heat + 0.5 * uLevel) * awake + 0.12 * uSleep) * mix (0.75, 1.0, min (glowK, 1.0)) * (0.8 + 0.2 * glowK);
    float iris = exp (-length (e / vec2 (ew * 0.8, 0.032)) * 2.0);
    float em = (1.0 - smoothstep (-aaE - 0.006, aaE + 0.004, dEye)) * (0.35 + 1.2 * iris) * eI * Fe * keep;
    em += (eyB.x * bright.x * (0.3 + 1.1 * eyB.z) + eyK.x * bright.y * (0.3 + 1.1 * eyK.z) + eyC.x * bright.z * (0.3 + 1.1 * eyC.z)) * eI * 0.85;
    // the throat (and its glow already in the cloud before the mouth has a shape)
    float mouthShape = 1.0 - smoothstep (-aa - 0.006, aa + 0.006, dMouth);
    float subPulse = uSub * uSubLevel * (0.5 + 0.5 * sin (uSubPhase * 6.2832 * 3.0));
    float tg = exp (-length ((h - vec2 (0.0, mouthMid)) / vec2 (mw * 0.75, open * 0.6 + 0.015)) * 1.5);
    float throatI = (0.35 + 1.3 * heat + 0.6 * uLevel + 0.8 * subPulse) * awake * mix (0.8, 1.0, min (glowK, 1.0));
    em += tg * throatI * mouthShape * Fm * keep * 0.9;
    em += exp (-max (dMouth, 0.0) / 0.03) * throatI * 0.3 * Fe * (1.0 - Fm) * keep;
    // the vocoder's ridges
    // (broad and soft-ended: glowing ridges in the smoke, never strokes)
    float frillCore = (1.0 - smoothstep (0.0, min (1.6 * halfW, 0.5), abs (fu))) * smoothstep (r0 - 0.04, r0, fr)
                    * (1.0 - smoothstep (r0 + 0.6 * flen, r0 + flen + 0.03, fr)) * (1.0 - smoothstep (1.65, 1.8, fang));
    em += uGravelOn * frillCore * (0.2 + 1.0 * band) * (1.0 - along * 0.6) * (0.3 + 0.7 * awake) * (0.25 + 0.75 * Fh) * keep;
    // the overtone's ribs, one per harmonic
    float ribs = 0.0;
    for (int k = 0; k < 5; k++)
    {
        float fkk = float (k);
        float hk = clamp (uOvHarm[k], 0.0, 1.0);
        float yk = -0.215 - chinDrop - 0.03 * jawM - 0.032 * fkk;
        float xr = h.x / (0.11 - 0.012 * fkk);
        float dr = abs (h.y - (yk + 0.035 * xr * xr));
        float flick = 0.85 + 0.15 * sin (t * (4.0 + 1.0 * fkk) + fkk * 1.7);
        ribs += exp (-dr / 0.006) * (1.0 - smoothstep (0.8, 1.0, xr)) * hk * flick;
    }
    em += ribs * clamp (ovA * 1.6, 0.0, 1.0) * 1.4;
    // the limiter straining: the temples pulse
    vec2 tc = (h - vec2 (0.225, 0.165) * vec2 (1.0, cran)) / vec2 (0.09, 0.075);
    float inBody = 1.0 - smoothstep (-0.01, 0.01, dEdge);
    em += exp (-dot (tc, tc)) * uStrain * (0.25 + 1.2 * uStrainPulse) * awake * Fh * inBody * 0.9;
    // bioluminescent dots along the wing edges and the spines
    if (uEvoPal.w > 0.001)
    {
        // (well jittered, most cells empty, each dot its own size and strength, in noise-shaped
        // clusters: a lattice of even dots reads as a pattern laid over the smoke)
        vec2 bc = wq * 26.0;
        vec2 bcell = floor (bc);
        float bh = hash12 (bcell + 31.7);
        vec2 bo = vec2 (hash12 (bcell + 5.3), hash12 (bcell + 9.1)) * 0.7 + 0.15;
        float br = 0.06 + 0.08 * hash12 (bcell + 2.9);
        float bdd = length (fract (bc) - bo);
        float zoneB = smoothstep (0.42, 0.72, vnoise (wq * 3.1 + vec2 (1.7, 4.2)));
        // (sparse along the spines: a full row of them reads as a dotted line)
        float along2 = exp (-abs (dEdge + 0.012) / 0.02) + 0.3 * smoothstep (0.02, -0.005, dCrown);
        float dot1 = step (0.80, bh) * (1.0 - smoothstep (0.3 * br, br, bdd)) * along2 * zoneB * (0.4 + 0.6 * hash12 (bcell + 6.6));
        float pulse = 0.75 + 0.25 * sin (t * (0.8 + 1.7 * bh) + bh * 40.0);
        em += dot1 * pulse * uEvoPal.w * 0.8 * Fx * keep * (0.4 + 0.6 * awake);
    }
    // the clipper's sparks: seeds along the edge band, a new set per spark
    if (uSpark.y > 0.001)
    {
        // (round, soft points inside their cells: a lit cell would read as a square speck)
        vec2 sp = h * 60.0;
        vec2 sc = floor (sp);
        float sh = hash12 (sc + uSpark.x * 7.31);
        vec2 so = vec2 (hash12 (sc + 3.1), hash12 (sc + 8.7)) * 0.5 + 0.25;
        float sd = length (fract (sp) - so);
        em += step (0.985, sh) * (1.0 - smoothstep (0.06, 0.28, sd)) * exp (-abs (dEdge) * 70.0) * uSpark.y * 3.0 * awake;
    }

    // ---- red pigment: splashes on the head, under the jaw and on the wings; the logo's cherry
    float red = 0.0;
    if (uEvoPal.y > 0.001 || egg > 0.001)
    {
        float dRed = sdEllipse (rot (0.5) * (h - vec2 (0.13, 0.34 * cran)), vec2 (0.09, 0.04));
        dRed = smin (dRed, sdEllipse (h - vec2 (0.0, chinY + 0.07), vec2 (0.12, 0.04)), 0.04);
        dRed = min (dRed, sdEllipse (rot (-0.25) * (wq - vec2 (0.66, -0.12)), vec2 (0.13, 0.06)));
        float rn = seedNoise (h * 14.0, 1.0) - 0.5;
        dRed += (edgeN - 0.5) * 0.07 + rn * 0.03;
        float redCore = 1.0 - smoothstep (-0.03, 0.012 + 0.02 * sigma, dRed);
        float redRim = exp (-abs (dRed + 0.006) / (0.008 + 0.006 * sigma));
        red = clamp (redCore * (0.45 + 0.25 * C) + redRim * 0.25, 0.0, 1.0) * smoothstep (0.05, 0.6, uEvoPal.y) * Fh * keep
            * smoothstep (0.1, 0.6, inBody);
        red = max (red, (1.0 - smoothstep (-0.012, 0.006, dLogoC)) * smoothstep (0.4, 1.0, gl));
    }

    gl_FragColor = vec4 (dV, rho, em, red);
}
)GLSL";

const char* const c_compPre = R"GLSL(
varying vec2 vUv;

uniform vec2 uOutRes;
uniform sampler2D uDye;
uniform sampler2D uLight;
uniform sampler2D uCF;
uniform sampler2D uV;
uniform sampler2D uNoise;
uniform sampler2D uBloom;
uniform vec4 uDyeS;
uniform vec4 uLightS;
uniform vec4 uCFS;
uniform vec4 uSimS;
uniform vec4 uBloomS;
uniform vec2 uDyeRes;     // the active dye size
uniform vec2 uCFTexel;    // one active creature-field texel, in uv
uniform vec2 uSimTexel;   // one sim texel, in uv
uniform vec4 uComp;       // cubic dye (1/0), sim texels to view heights, flow period (s), detail tiles per view height
uniform vec4 uLightDir;
uniform vec4 uGrain;      // grain offset xy, grain on, bloom gain
uniform vec3 uEyeCol;
uniform vec3 uThroatCol;

vec4 dyeLin (vec2 uv) { return texture2D (uDye, min (uv * uDyeS.xy, uDyeS.zw)); }

// Bicubic B-spline from four bilinear taps: hides the dye's texels at full resolution.
vec4 dyeCubic (vec2 uv)
{
    vec2 st = uv * uDyeRes - 0.5;
    vec2 i = floor (st);
    vec2 f = st - i;
    vec2 f2 = f * f;
    vec2 f3 = f2 * f;
    vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    vec2 w3 = f3 / 6.0;
    vec2 g0 = w0 + w1;
    vec2 g1 = w2 + w3;
    vec2 h0 = (i - 0.5 + w1 / g0) / uDyeRes;
    vec2 h1 = (i + 1.5 + w3 / g1) / uDyeRes;
    return g0.y * (g0.x * dyeLin (h0) + g1.x * dyeLin (vec2 (h1.x, h0.y)))
         + g1.y * (g0.x * dyeLin (vec2 (h0.x, h1.y)) + g1.x * dyeLin (h1));
}

vec4 cfAt (vec2 uv) { return texture2D (uCF, min (uv * uCFS.xy, uCFS.zw)); }

// The light at full resolution, softened: four bilinear taps a texel apart (it has the dye's size).
// Light in smoke is soft; read sharply, the light's texel-sized rims magnify into jagged lines.
vec4 ltLin (vec2 uv) { return texture2D (uLight, min (uv * uLightS.xy, uLightS.zw)); }
vec4 ltSoft (vec2 uv)
{
    vec2 o = 1.0 / uDyeRes;
    return 0.25 * (ltLin (uv + o) + ltLin (uv - o) + ltLin (uv + vec2 (o.x, -o.y)) + ltLin (uv + vec2 (-o.x, o.y)));
}
)GLSL";

const char* const c_compMainPre = R"GLSL(

// The full-resolution composite: the lit medium with flow-moved detail, subtractive ink, the few
// crisp parts (eye cores, teeth, lip edge, plates), the front fog, bloom, grain and the card.
void main()
{
    vec2 frag = vUv * uOutRes;
    vec2 p = (vUv - 0.5) * vec2 (uAspect, 1.0);
    float px = 1.0 / uOutRes.y;

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

    float F2 = F * F;
    float glowK = uEvoPal.z;

    // ---- the card: dark scope and the fold down the middle (the grid goes on after the medium,
    // only where the smoke leaves a gap)
    // (a far, dim sky rather than a black card: under the high-key grade, any gap the smoke leaves
    // must read as distant cloud, never as a dark hole or a dotted rim)
    vec3 col = mix (uColScope, uColCloudLit, 0.45);
    vec2 gd = abs (fract (p * 8.0 + 0.5) - 0.5) / (8.0 * px);
    float grid = 1.0 - smoothstep (0.0, 1.25, min (gd.x, gd.y));
    col += uColGrid * exp (-abs (p.x) / (2.0 * px)) * 0.5;

    // ---- the medium (a third of its mirror image blended in: still a blot, never lopsided)
    vec2 uvM = vec2 (1.0 - vUv.x, vUv.y);
    vec4 D = max (uComp.x > 0.5 ? dyeCubic (vUv) : dyeLin (vUv), vec4 (0.0));
    D = mix (D, max (uComp.x > 0.5 ? dyeCubic (uvM) : dyeLin (uvM), vec4 (0.0)), 0.3);
    vec4 Lt = uComp.x > 0.5 ? ltSoft (vUv) : ltLin (vUv);
    Lt.rg = mix (Lt.rg, (uComp.x > 0.5 ? ltSoft (uvM) : ltLin (uvM)).rg, 0.3);
    vec4 cf = cfAt (vUv);
    // (the slow copy of the velocity: the detail must not jump with the fluid's frame-to-frame jitter)
    // (four taps half a texel apart: the sim is coarse, and a bilinear velocity bends the moved
    // detail at every texel edge, which shows as steps)
    vec2 sh = 0.5 * uSimTexel;
    vec2 vel = 0.25 * (texture2D (uV, min ((vUv + sh) * uSimS.xy, uSimS.zw)).zw + texture2D (uV, min ((vUv - sh) * uSimS.xy, uSimS.zw)).zw
                     + texture2D (uV, min ((vUv + vec2 (sh.x, -sh.y)) * uSimS.xy, uSimS.zw)).zw
                     + texture2D (uV, min ((vUv + vec2 (-sh.x, sh.y)) * uSimS.xy, uSimS.zw)).zw);
    vel *= min (1.0, 15.0 / max (length (vel), 1.0e-3));
    float fd = fwidth (cf.x);

    // flow-moved detail: two phases of the baked noise carried along the velocity, desynchronised
    // per place so nothing pulses
    float T = uComp.z;
    // (rotated, so the tile's repeat never lines up with the screen)
    vec2 nc = mat2 (0.866, 0.5, -0.5, 0.866) * p * uComp.w;
    float off = texture2D (uNoise, nc * 0.11 + vec2 (0.3, 0.7)).r * 2.0;
    float ph0 = fract (t / T + off);
    float ph1 = fract (t / T + off + 0.5);
    vec2 fv = vel * uComp.y * uComp.w;
    vec4 n0 = texture2D (uNoise, nc - fv * (ph0 - 0.5) * T);
    vec4 n1 = texture2D (uNoise, nc * 1.07 + vec2 (0.37, 0.61) - fv * (ph1 - 0.5) * T);
    float w0 = 1.0 - abs (2.0 * ph0 - 1.0);
    float w1 = 1.0 - w0;
    float wn = inversesqrt (w0 * w0 + w1 * w1);
    float n = (w0 * n0.r + w1 * n1.r - 0.5) * wn + 0.5;
    float rdg = (w0 * n0.a + w1 * n1.a - 0.5) * wn + 0.5;
    vec2 gn = (w0 * (n0.gb - 0.5) + w1 * (n1.gb - 0.5)) * wn;
    // streaks along the flow: the fine detail smeared along the velocity, so the swirl's lines show
    float spd = length (vel);
    vec2 vdir = vel / max (spd, 1.0e-3) * clamp (spd / 5.0, 0.0, 1.0) * 0.05;
    vec2 sc = nc * 0.9 + vec2 (0.71, 0.13);
    vec2 so0 = -fv * 0.9 * (ph0 - 0.5) * T;
    vec2 so1 = -fv * 0.9 * (ph1 - 0.5) * T + vec2 (0.29, 0.47);
    float st0 = (texture2D (uNoise, sc + so0 + vdir).a + texture2D (uNoise, sc + so0).a + texture2D (uNoise, sc + so0 - vdir).a) / 3.0;
    float st1 = (texture2D (uNoise, sc + so1 + vdir).a + texture2D (uNoise, sc + so1).a + texture2D (uNoise, sc + so1 - vdir).a) / 3.0;
    float streak = (w0 * st0 + w1 * st1 - 0.5) * wn + 0.5;

    float cloud = D.r;
    float C = clamp (D.r, 0.1, 0.95);
)GLSL";

const char* const c_compPost = R"GLSL(

    float gl = egg > 0.001 ? clamp (egg * 1.6 - 0.3 + 0.3 * (cloud - 0.5) - 0.25 * length (p), 0.0, 1.0) : 0.0;
    float keep = 1.0 - gl;
    float d = cf.x;
    float dInk = d / scale;
    float eI = ((0.75 + 1.7 * heat + 0.5 * uLevel) * awake + 0.12 * uSleep) * mix (0.75, 1.0, min (glowK, 1.0)) * (0.8 + 0.2 * glowK);

    // ---- the front fog: an advected layer, denser low on the face and at the silhouette, thinner over the eyes
    float lowerFace = smoothstep (mY, mY - 0.25, h.y) * (1.0 - smoothstep (0.25, 0.6, h.x)) * keep * Fm;
    float fogIn = D.g * (1.0 + 0.6 * lowerFace + 0.4 * exp (-abs (d) / 0.05) * Fh);
    vec2 eRel = (h - ec) / 0.085;
    float eyeGlow = clamp (eI * 0.5, 0.0, 1.0) * Fe * keep;
    float thinEyes = mix (0.12, 0.45, eyeGlow) * exp (-dot (eRel, eRel)) * smoothstep (0.0, 0.2, eyeGlow);
    float fogFront = min (fogA * 1.7, 0.6) * smoothstep (0.12, 0.75, fogIn) * (1.0 - thinEyes);
    fogFront = clamp (fogFront + 0.15 * uGateClosed * awake * lowerFace, 0.0, 0.6); 

    // ---- what the stage and the character do to the medium
    float stageK = clamp (uSeedB.w, 0.0, 1.0);          // early: wispy, translucent; late: dense, inky
    vec4 chr = uEvoChar;                                // fluid, beast, eldritch, deep
    float fray = clamp (1.0 + 1.3 * chr.y + 0.7 * chr.z - 0.6 * chr.w - 0.4 * chr.x, 0.4, 2.4);

    // ---- a far layer: the same smoke, larger and drifting its own way, dim behind the rest (depth)
    {
        vec2 fuv = 0.5 + (vUv - 0.5) * 0.74 + vec2 (0.035 * sin (t * 0.019), 0.025 * cos (t * 0.023));
        float Df = 0.5 * (dyeLin (fuv).r + dyeLin (vec2 (1.0 - fuv.x, fuv.y)).r);
        float farA = (1.0 - exp (-1.3 * Df)) * 0.4;
        vec3 farCol = mix (uColScope, uColCloud, 0.55) * (0.75 + 0.35 * n);
        col = mix (col, farCol, farA);
    }

    // ---- density: the back cloud (eroded where thin, wound into filaments along the flow) and the
    // body condensed from it
    float rr = D.r;
    float lo = 0.3 * (1.0 - n) * (1.0 - 0.6 * min (rr, 1.0));
    float rhoB = clamp ((rr - lo) / (1.0 - lo), 0.0, 2.5);
    rhoB *= mix (1.0, 0.55 + 0.9 * rdg, (1.0 - smoothstep (0.15, 0.7, rr)) * (0.5 + 0.5 * inky));
    // filaments: the flow-smeared detail's ridges, strongest where the smoke is thin
    // (a contrast curve on the smeared detail, not its ridges: a noise's mid-level ridges close into
    // little rings, which read as bubbles)
    float fil = smoothstep (0.3, 0.8, streak);
    float thinB = 1.0 - smoothstep (0.25, 0.9, rr);
    rhoB *= mix (0.85 + 0.3 * streak, 0.65 + 0.7 * fil + 0.3 * (n - 0.5), thinB * (0.5 + 0.25 * chr.z - 0.2 * chr.w));

    // the body's edge is a band of thinning smoke, not a line: over the outer part of the profile
    // the density falls away along the moving detail, so the cloud behind shows through in wisps
    float dE = d + (0.6 * (n - 0.5) + 0.5 * (streak - 0.5)) * (0.016 + 0.012 * uEvoInk.y) * fray * (1.0 - 0.4 * F);
    float wOut = max (1.2 * fd, 1.5 * px);
    float morphW = sin (3.1416 * gl);   // (the morph into the logo and back: its front is a wide, soft band)
    float wIn = mix (0.065, 0.032, stageK) * (0.8 + 0.3 * fray) * scale * (1.0 + 4.0 * morphW);
    float eIn = clamp (-dE / wIn, 0.0, 1.0);
    float frayN = (1.1 * (n - 0.5) + 0.9 * (streak - 0.5)) * fray;
    // (a soft response to the fray, not a threshold: thresholded noise leaves isolated specks of
    // body outside the edge, which read as a dotted rim)
    float abF = smoothstep (-0.35, 0.75, 1.3 * eIn - 0.15 + 0.85 * frayN * (1.0 - eIn));
    abF = abF * abF * (3.0 - 2.0 * abF) * (1.0 - smoothstep (-0.012 - 0.10 * morphW, 0.022 + 0.012 * fray + 0.06 * morphW, dE));
    float abU = 1.0 - smoothstep (-0.025 - 0.10 * morphW, 0.025 + 0.06 * morphW, dE);   // (still cloud: a wide soft edge)
    // (nothing formed, no body: the faint field left while it sleeps must not mark the cloud)
    float ab = mix (abU, abF, smoothstep (0.4, 1.0, F)) * smoothstep (0.02, 0.3, F);
    float rhoBody = max (rhoB * ab, cf.y * F2 * ab) * (1.0 - 0.6 * F * smoothstep (0.3, 0.9, fogFront));
    rhoBody *= 1.0 + 0.45 * (n - 0.5) * (1.0 - 0.6 * uEvoInk.x * F2);
    float rhoT = max (rhoB, rhoBody);
    vec2 cx = vec2 (4.0 * uCFTexel.x, 0.0);
    vec2 cy = vec2 (0.0, 4.0 * uCFTexel.y);
    float rhoBlur = 0.25 * (cfAt (vUv + cx).y + cfAt (vUv - cx).y + cfAt (vUv + cy).y + cfAt (vUv - cy).y);

    // ---- light, as participating media: the shared light pass marches towards the sun through the
    // cloud and the body (self-shadow: dark cores, bright rims), plus sky and a forward-scatter lobe.
    // No bump from the density's slope: that is what made the smoke read as clay.
    vec2 L2 = uLightDir.xy;
    float sun = Lt.r;
    float sky = Lt.g;
    float heatD = D.b;
    float halfO = clamp (rhoT, 0.0, 1.0);
    float shade = 0.8 * sun + 0.3 * sky;
    vec3 voidC = uColCloud * 0.12;
    vec3 lit = mix (voidC, uColCloud * (0.45 + 0.35 * sky), smoothstep (0.08, 0.65, shade));
    lit = mix (lit, uColCloudLit * 1.3, smoothstep (0.5, 1.15, shade));
    // thin smoke lit from behind glows at its edges (forward scattering)
    float thinT = exp (-2.5 * rhoT);
    lit += uColCloudLit * 0.35 * clamp (sun, 0.0, 1.2) * thinT * (1.0 - thinT) * 4.0 * (0.6 + 0.4 * fil);
    lit += uColUnderglow * (0.08 + 0.25 * heat * awake + 0.8 * heatD) * (1.0 - 0.5 * clamp (sun, 0.0, 1.0));
    // (a ceiling on the cloud's brightness: the creature keeps its contrast on both sides)
    float litL = dot (lit, vec3 (0.3, 0.59, 0.11));
    lit *= min (1.0, 0.66 / max (litL, 1.0e-3));
    // dense, pooled smoke keeps its moving texture (a saturated pocket must never read as a flat slab)
    lit *= 1.0 + (0.40 * (n - 0.5) + 0.25 * (streak - 0.5)) * smoothstep (0.5, 1.1, rr);

    // ---- ink optics (subtractive, so overlaps deepen the way pigment does)
    vec3 inkCol = vec3 (0.050, 0.055, 0.085) + uColPitch * 0.05;
    vec3 Kink = -log (max (inkCol, vec3 (0.02)));
    float inkK = (0.12 + 0.6 * uEvoInk.x) * mix (0.35, 1.15, stageK) * (1.0 + 0.35 * chr.w - 0.2 * chr.x);
    // (asleep, what is left of it is a dim hump of ink in the smoke, not a pale patch of cloud)
    float hInk = inkK * max (F2, 0.6 * F * Fe) * rhoBody;
    hInk *= 1.0 + 0.7 * uForm.w * clamp (3.0 * (cf.y - rhoBlur), 0.0, 1.0);    // pooled at the edge, inside (gently: no drawn rim)
    float thinI = 1.0 - smoothstep (0.2, 0.6, rhoBody);
    float paper = texture2D (uNoise, frag * vec2 (0.5, 0.5 / 6.0) / 256.0).a;
    hInk *= 1.0 + 0.6 * (paper - 0.5) * thinI * smoothstep (-0.005, -0.03, d) * (0.4 + uEvoInk.z);   // (inside only: never a dotted rim)
    // smoke moving inside the ink: thicker and thinner along the swirl, more so at the frayed edge
    hInk *= 0.7 + 1.3 * (n - 0.5) + 0.45 * (streak - 0.5) + 0.35 * (fil - 0.4) * (1.0 - eIn);
    vec3 med = lit * exp (-Kink * max (hInk, 0.0));
    // the ink's pigment carried off the body into the cloud (wisps of dark smoke); red on the
    // red-black forms
    vec3 redN = uColRedInk / max (max (uColRedInk.r, uColRedInk.g), max (uColRedInk.b, 1.0e-3));
    vec3 Kred = -log (max (redN, vec3 (0.03)));
    float redK = smoothstep (0.05, 0.6, uEvoPal.y);
    // (kept off the body only while it is a body: dispersed, its inside is cloud like the rest, or it
    // shows as a pale disc where the face was)
    // (the mask is the smooth silhouette, reaching a little past it, not the frayed body: through the
    // fray's noise, the pigment pooled at a still edge shows as a dotted dark rim)
    float bodyMask = max (ab, 1.0 - smoothstep (-0.01, 0.05, d));
    float pig = D.a * (1.0 - bodyMask * smoothstep (0.3, 0.9, F)) * (0.8 + 0.25 * fil + 0.3 * (n - 0.5));
    med *= exp (-mix (Kink, Kred * 0.8, redK * 0.7) * pig * 1.1);
    med *= exp (-Kred * cf.w * F2 * (0.7 + 0.5 * uEvoPal.y));
    if (uEvoPal.x > 0.001)
    {
        float wA = smoothstep (0.35, 0.75, seedNoise (h * 2.2, 0.3)) * smoothstep (0.35, 0.7, h.x);
        float wB = exp (-dot ((h - vec2 (0.20, -0.05)) / vec2 (0.12, 0.08), (h - vec2 (0.20, -0.05)) / vec2 (0.12, 0.08)))
                 + exp (-dot ((h - vec2 (0.0, 0.32)) / vec2 (0.22, 0.08), (h - vec2 (0.0, 0.32)) / vec2 (0.22, 0.08)));
        vec3 kA = -log (max (uColWashA / max (max (uColWashA.r, uColWashA.g), max (uColWashA.b, 1.0e-3)), vec3 (0.05)));
        vec3 kB = -log (max (uColWashB / max (max (uColWashB.r, uColWashB.g), max (uColWashB.b, 1.0e-3)), vec3 (0.05)));
        med *= exp (-(kA * wA * 0.8 + kB * clamp (wB, 0.0, 1.0) * 0.7) * uEvoPal.x * 0.7 * halfO * keep);
        med *= 1.0 + 0.5 * uEvoPal.x * (wA + 0.5 * clamp (wB, 0.0, 1.0)) * halfO * keep;
    }
    // a faint wet glint where the ink is thick and lit (the gloss of the late, liquid forms)
    med += mix (uColLiquid, uColText, 0.6) * smoothstep (0.7, 1.3, sun) * eIn * (0.04 + 0.1 * lq) * uEvoInk.w * uEvoInk.x * F2 * ab * (1.0 - 0.65 * heat);
    // tame: the crown dims while the top end is held back
    med *= 1.0 - 0.35 * uTame * smoothstep (0.12, 0.28, h.y) * ab;
    // swirl: a faint iridescence carried in the medium at the notch
    if (uSwirlAmt > 0.001)
    {
        float sy = mix (-0.30, 0.32, uSwirlNotch);
        vec3 irid = 0.5 + 0.5 * cos (6.2832 * (vec3 (0.0, 0.33, 0.67) + n * 1.5 + h.y * 1.5 + t * 0.15));
        med += mix (uColSwirl, irid, 0.5) * exp (-pow ((h.y - sy) / 0.14, 2.0)) * uSwirlAmt * 0.05 * (0.5 + n) * halfO * awake * Fh;
    }

    // transmittance: extinction from density (the body is as dense as the cloud while it is cloud;
    // formed, it is thicker in its core and thins through the edge band)
    float sigB = mix (2.2, mix (1.6, 6.0, stageK) * (1.0 + 0.25 * chr.w - 0.15 * chr.x), F);
    float alpha = 1.0 - exp (-2.2 * rhoB * (1.0 - ab) - sigB * rhoBody);
    col = mix (col, med, alpha);
    // the grid: only through the clear gaps
    col = mix (col, uColGrid, grid * 0.40 * (1.0 - alpha) * (1.0 - alpha) * (1.0 - smoothstep (0.05, 0.4, rhoB)));

    // ---- emission: the eyes and the throat through the ink (red reaches furthest), heat, parts' glow
    vec3 eyeCol = uEyeCol;
    vec3 deepen = exp (-2.0 * rhoBody * vec3 (0.3, 1.0, 1.6));
    col += Lt.b * eyeCol * (1.0 + 1.5 * fogFront) * mix (vec3 (1.0), deepen, 0.7) * 0.55;
    vec3 throatCol = uThroatCol;
    // (the throat's light reaches out into the smoke round the mouth: lit wisps, not a hotter core)
    col += Lt.a * throatCol * (0.18 + 0.30 * halfO);
    col += heatD * heatD * mix (vec3 (1.0, 0.45, 0.18), uColRoar, 0.5) * 0.6;
    vec3 tint = eyeCol;
    {
        vec2 fc = vec2 (0.0, -0.13 - chinDrop * 0.6 - 0.03 * jawM);
        vec2 fv2 = h - fc;
        float r0f = 0.17 + 0.04 * sizeJ;
        float inFrill = step (atan (fv2.x, -fv2.y), 1.8) * smoothstep (r0f - 0.05, r0f, length (fv2)) * step (0.001, uGravelOn);
        float inRibs = smoothstep (chinY + 0.08, chinY, h.y) * (1.0 - smoothstep (0.08, 0.13, h.x)) * step (0.001, uOvLevel);
        float inBio = smoothstep (0.4, 0.6, h.x) * step (0.001, uEvoPal.w);
        vec2 tcm = (h - vec2 (0.225, 0.165) * vec2 (1.0, cran)) / vec2 (0.09, 0.075);
        float inTemple = exp (-dot (tcm, tcm)) * step (0.001, uStrain);
        tint = mix (tint, uColVocoder, inFrill);
        tint = mix (tint, mix (uColOvertone, vec3 (1.0), 0.15), inRibs);
        tint = mix (tint, mix (mix (uColWashA, uColWashB, 0.5), vec3 (0.55, 0.95, 1.0), 0.35), inBio);
        tint = mix (tint, mix (uColOutput, uColRoar, 0.35), inTemple);
    }
    col += cf.z * tint * 0.45;
    // asleep: a low ember in the throat, breathing, deep in the smoke (something is still there)
    {
        vec2 qe = (h - vec2 (0.0, mY - 0.03)) / vec2 (0.13, 0.06);
        float breathE = 0.6 + 0.4 * clamp (0.5 + 0.4 * uHead.w, 0.0, 1.0);
        col += throatCol * exp (-dot (qe, qe)) * uSleep * keep * 0.07 * breathE * (1.0 - 0.5 * fogFront);
    }

    // ---- the crisp parts (spatial branches; analytic widths, no derivatives in here)
    float aaC = 1.5 * px / scale;
    float eyeM = 0.0;      // (the crisp eye and the maw, for the grade and the mist below)
    float mawM = 0.0;
    float mawCore = 0.0;
    // eyes: iris and slit in the almond's soft hole
    if (h.x < 0.32 && h.y > -0.06 && h.y < 0.34)
    {
        float aaE2 = aaC + 0.004 * sigma;
        float eye = (1.0 - smoothstep (-aaE2, aaE2, dEye)) * Fe * keep;
        float iris = exp (-length (e / vec2 (ew * 0.8, 0.032)) * 2.0);
        // the pupil: a slit across the almond that narrows to points at the lids, a dark soft core;
        // the catch-light is a small round spot off to one side, under it (never a bar across it)
        float ey = (h.y - ec.y) / max (eh * 1.1, 1.0e-3);
        float sw = (0.0035 + 0.0045 * (1.0 - heat) + 0.003 * sigma) * sqrt (clamp (1.0 - ey * ey, 0.0, 1.0));
        float slit = 1.0 - smoothstep (0.3 * sw, sw + 0.002, abs (h.x - ec.x));
        vec3 eyeFill = eyeCol * (0.35 + 1.2 * iris) * eI;
        vec2 clv = e - vec2 (-0.38 * ew, 0.3 * eh);
        eyeFill += vec3 (1.0, 0.95, 0.8) * exp (-dot (clv, clv) / 3.0e-5) * 0.5 * eI * eyeOpen;
        eyeFill = mix (eyeFill, vec3 (0.015, 0.0, 0.004), slit * eyeOpen * (0.82 + 0.13 * (1.0 - sigma)));
        eyeFill = mix (eyeFill, eyeFill * vec3 (1.0, 0.75, 0.7), fogFront);
        col = mix (col, eyeFill, eye);
        eyeM = eye;
        // asleep: a seam of glow where the lids meet
        col += eyeCol * exp (-abs (e.y) / 0.008) * (1.0 - smoothstep (ew * 0.15, ew * 0.7, abs (e.x))) * 0.12 * uSleep * Fe * keep;
        vec3 brightX = smoothstep (0.15, 0.4, openX) * Fe * keep;
        if (openX.x + openX.y + openX.z > 0.001)
        {
            vec3 exCol = mix (eyeCol, uColWashB, 0.25 * uEvoPal.w);
            float exI = eI * 0.85;
            col = mix (col, exCol * (0.3 + 1.1 * eyB.z) * exI, eyB.x * brightX.x);
            col = mix (col, exCol * (0.3 + 1.1 * eyK.z) * exI, eyK.x * brightX.y);
            col = mix (col, exCol * (0.3 + 1.1 * eyC.z) * exI * 1.1, eyC.x * brightX.z);
        }
    }

    // the maw: a hot throat at the back, the teeth, the muzzle
    // (a bounding box for speed only: everything drawn inside it, the muzzle's feathered outline
    // included, has faded to nothing well before its edges, at every opening)
    float toothCov = 0.0;   // (the teeth stay dark against the glow: later glows and bloom skip them)
    if (h.x < mw * 1.25 + 0.02 && h.y < mY + 0.07 + 0.25 * open && h.y > mY - 1.1 * open - 0.09)
    {
        float aaM = aaC + 0.006 * sigma;
        float mouthShape = 1.0 - smoothstep (-aaM, aaM, dMouth);
        float mouth = mouthShape * Fm * keep;
        float subPulse = uSub * uSubLevel * (0.5 + 0.5 * sin (uSubPhase * 6.2832 * 3.0));
        float tg = exp (-length ((h - vec2 (0.0, mouthMid)) / vec2 (mw * 0.75, open * 0.6 + 0.015)) * 1.5);
        float throatI = min ((0.35 + 1.3 * heat + 0.6 * uLevel + 0.8 * subPulse) * awake * mix (0.8, 1.0, min (glowK, 1.0)), 1.4);
        vec3 throatFloor = mix (vec3 (0.05, 0.004, 0.01), uColCloud * 0.25, 0.6);
        // a cavity, not a lit dome: the glow sits low and back, the roof under the upper teeth and the
        // corners fall into shadow, and the smoke moving inside breaks it up
        float yRel = clamp ((h.y - yL) / max (yU - yL, 1.0e-3), 0.0, 1.0);
        float core = exp (-(h.x * h.x) / (mw * mw * 0.18) - (yRel - 0.36) * (yRel - 0.36) / 0.09);
        float cavity = (1.0 - 0.75 * smoothstep (0.45, 1.0, yRel)) * (1.0 - 0.65 * smoothstep (0.3, 0.95, h.x / max (mw, 1.0e-3)));
        vec3 throat = throatFloor + throatCol * 0.45 * (1.0 - exp (-(0.2 * tg + 0.9 * core) * throatI * cavity * (0.8 + 0.4 * n) / 0.45));
        float lipShade = smoothstep (0.0, 0.01 + 0.012 * sigma, min (yU - h.y, h.y - yL));
        throat = mix (mix (col, throatFloor, 0.5), throat, 0.3 + 0.7 * lipShade);
        throat += uColGrowl * (1.0 - lipShade) * uGrowl * uGrowlMod * 0.4 * awake;
        float ovR = uOvLevel * awake * Fh * keep;
        throat *= 1.0 - 0.6 * uOvReplace * ovR;
        col = mix (col, throat, mouth);
        col += throatCol * tg * mouth * throatI * fogFront * 0.2;
        mawM = mouth;
        mawCore = clamp (core * throatI * cavity, 0.0, 1.0) * mouth;

        // teeth: occluders lit by the same light, translucent at the tips, no strokes
        float tw = mw / 5.0;
        float teethZone = 1.0 - smoothstep (mw * 0.92, mw * 1.0, h.x);
        float baseLen = (0.024 + 0.022 * size) * uEvoMaw.y;
        vec3 bone = mix (vec3 (0.80, 0.75, 0.66), uColText, 0.3);
        bone = mix (bone, mix (uColCloud, uColCloudLit, 0.7), 0.2 + 0.15 * sigma);
        bone *= (0.6 + 0.45 * clamp (sun, 0.0, 1.4)) * (1.0 - 0.15 * heat * awake);   // (pale against the cavity: the teeth must read)
        vec3 glowOnBone = throatCol * tg * (0.2 + 0.3 * heat) * awake;
        vec3 boneCloud = mix (uColCloud, uColCloudLit, 0.55);
        float softT = 1.0 + 3.0 * sigma;
        float teethF = Fm * keep * (1.0 - 0.5 * fogFront);
        float deessGlint = uDeess * (0.6 + 0.4 * sin (t * 3.0 + h.x * 40.0)) * awake;
        float ta = aaC * softT;

        float u3 = h.x / (tw * 0.72) + 0.25;
        float f3 = fract (u3) - 0.5;
        float tc3 = 1.0 - pow (abs (f3) * 2.0, 1.3);
        float back = max ((yU - 0.006 - baseLen * 0.55 * tc3) - h.y, h.y - yU);
        back = min (back, max (h.y - (yL + 0.004 + baseLen * 0.45 * tc3), yL - h.y));
        float backTeeth = (1.0 - smoothstep (-ta, ta, back)) * mouthShape * teethZone;
        col = mix (col, bone * 0.22 + glowOnBone * 0.3, backTeeth * teethF * Fm * 0.6);
        toothCov = backTeeth * teethF * Fm * 0.6;
        col += uColDeess * backTeeth * deessGlint * 0.3 * teethF;

        if (uEvoMaw.z > 0.001)
        {
            float u4 = h.x / (tw * 0.45) + 0.5;
            float f4 = fract (u4) - 0.5;
            float tc4 = 1.0 - pow (abs (f4) * 2.0, 0.6);
            float L4 = baseLen * 1.15 * tc4 * uEvoMaw.z;
            float dN = max ((yU - 0.003 - L4) - h.y, h.y - (yU - 0.002));
            dN = min (dN, max (h.y - (yL + 0.003 + L4 * 0.8), (yL + 0.002) - h.y));
            float needles = (1.0 - smoothstep (-ta, ta, dN)) * mouthShape * teethZone;
            col = mix (col, bone * 0.55 + glowOnBone * 0.5, needles * teethF * 0.85);
            toothCov = max (toothCov, needles * teethF * 0.85);
            col += uColDeess * needles * deessGlint * 0.4 * teethF;
        }

        float u2 = h.x / tw + 0.5;
        float k2 = floor (u2);
        float f2 = fract (u2) - 0.5;
        vec3 tv2 = toothVar (k2, 2.0);
        float tc2 = 1.0 - pow (min (abs (f2) * 2.0 / tv2.z, 1.0), 1.25);
        tc2 = min (tc2, tv2.y);
        float L2t = baseLen * 0.8 * (1.05 - 0.07 * k2) * tv2.x;
        float dLow = max (h.y - (yL + L2t * tc2), (yL - 0.008) - h.y);
        float lowTeeth = (1.0 - smoothstep (-ta, ta, dLow)) * teethZone;
        float lowShade = clamp ((h.y - yL) / max (L2t, 1.0e-3), 0.0, 1.0);
        vec3 lowCol = bone * (0.45 + 0.55 * lowShade) * (0.75 + 0.25 * tc2) + glowOnBone * 0.5;
        lowCol = mix (lowCol, boneCloud, 0.45 * (1.0 - lowShade) * (0.4 + 0.6 * sigma));
        float lowA = lowTeeth * smoothstep (0.0, 0.02, -dInk + 0.02) * mix (1.0, 0.55, lowShade * sigma) * teethF;
        col = mix (col, lowCol, lowA);
        toothCov = max (toothCov, lowA);
        col += uColDeess * lowA * pow (tc2, 3.0) * deessGlint * 0.5;

        float u1 = h.x / tw;
        float k1 = floor (u1);
        float f1 = fract (u1) - 0.5;
        vec3 tv1 = toothVar (k1, 1.0);
        float tc1 = 1.0 - pow (min (abs (f1) * 2.0 / tv1.z, 1.0), 1.25);
        tc1 = min (tc1, tv1.y);
        float L1 = baseLen * (1.1 - 0.08 * k1) * tv1.x;
        float dUp = max ((yU - L1 * tc1) - h.y, h.y - (yU + 0.008));
        float upTeeth = (1.0 - smoothstep (-ta, ta, dUp)) * teethZone;
        float upShade = clamp ((yU - h.y) / max (L1, 1.0e-3), 0.0, 1.0);
        vec3 upCol = bone * (0.5 + 0.5 * upShade) * (0.75 + 0.25 * tc1) + glowOnBone * 0.35;
        upCol += vec3 (1.0) * pow (tc1, 8.0) * upShade * 0.12;
        upCol = mix (upCol, boneCloud, 0.45 * (1.0 - upShade) * (0.4 + 0.6 * sigma));
        float upA = upTeeth * smoothstep (0.0, 0.02, -dInk + 0.02) * mix (1.0, 0.55, upShade * sigma) * teethF;
        col = mix (col, upCol, upA);
        toothCov = max (toothCov, upA);
        col += uColDeess * upA * pow (tc1, 3.0) * (0.4 + 0.6 * upShade) * deessGlint;

        // ring: a muzzle of dark metal bands over the mouth, their flicker as light (small and slow).
        // Its value is the skin's own, darkened, and its oval outline is feathered into the skin, so
        // the mist veils it as it veils the body (an absolute metal colour inside a hard-edged
        // rectangle read as a pale tile pasted over the face)
        if (uRing > 0.001)
        {
            vec3 mz = ringMuzzle (h, mw, mY, open, uRingBody, 0.012 + 0.016 * sigma);
            float slats = 3.0 + 2.0 * uRingBody;
            float sv = fract (mz.y * slats);
            float slat = smoothstep (0.04, 0.2, sv) * smoothstep (0.96, 0.8, sv);
            float flk = 0.85 + 0.15 * uRingFlicker;
            float lit = 0.5 + 0.5 * clamp (sun, 0.0, 1.2);
            vec3 metal = col * (0.5 + 0.2 * (1.0 - sv)) * mix (vec3 (1.0), uColRing, 0.15);
            metal += uColRing * 0.05 * flk * (1.0 - sv) * lit;
            vec3 gap = col * 0.35 + uColRing * 0.03 * flk;
            float rivet = 0.0;
            for (int i = 0; i < 3; i++)
                rivet += ringRivet (h, mw, mY, open, uRingBody, float (i)) * uRingBody;
            vec3 plateCol = mix (gap, metal, slat) + vec3 (0.2) * rivet * lit;
            float plateA = min (clamp (0.3 + 1.4 * uRing, 0.0, 1.0), 0.85) * smoothstep (0.0, 0.03, uRing) * Fm * keep;
            col = mix (col, plateCol, mz.x * plateA);
        }
    }

    // ---- the front fog in front of it all, lit by the same light; the glows scatter in it
    vec3 fogCol = uColCloudLit * (0.20 + 0.45 * clamp (sun, 0.0, 1.5)) + uColCloud * 0.45 * sky + uColUnderglow * heat * awake * 0.12;
    col = mix (col, fogCol, fogFront);
    col += eyeCol * Lt.b * fogFront * 0.7;
    col += throatCol * Lt.a * fogFront * 0.2 * (1.0 - toothCov);

    // ---- bloom (not over the teeth): part under the mist, the rest scattered in it (below); tone
    vec3 bloomC = texture2D (uBloom, min (vUv * uBloomS.xy, uBloomS.zw)).rgb * uGrain.w * (1.0 - 0.85 * toothCov);
    col += bloomC * 0.15;
    col = 1.0 - exp (-1.6 * max (col, vec3 (0.0)));

    // ---- the grade: the scene's values laid onto the high-key cool ramp
    float lumS = dot (col, vec3 (0.3, 0.59, 0.11));
    col = atmoGrade (col, lumS);
    // the maw is a cavity: graded, its inside would lift to the mid-tones and read as a pale lens, so
    // it goes to the dark end of the ramp, its glow a small core low at the back (its warmth is laid
    // on over the mist, below); the teeth keep their own value over it
    col = mix (col, atmoTone (0.10 + 0.22 * mawCore), mawM * (1.0 - toothCov));

    // ---- the top mist, over everything (normal blend). The creature and the swirl read through it
    // as soft value shapes: the mist is shadowed by a wide blur of the body (never its outline, its
    // eyespots or its maw's edge, which would be drawn back into the mist), and carries the medium's
    // swirl in its own value; the glows and the deepest shadows come through a little.
    // (a ring of taps whose orientation turns slowly across the view, mirrored with the blot: a fixed
    // ring lays eight straight-edged copies of the head into the mist, which read as a soft polygon)
    float bodyW = 0.0;
    {
        float ra = sign (p.x) * 1.4 * (vnoise (vec2 (abs (p.x), p.y) * 2.7 + vec2 (3.3, 7.1)) - 0.5);
        vec2 a = vec2 (cos (ra), sin (ra)) * 0.07;
        vec2 b = vec2 (a.x - a.y, a.x + a.y) * 0.7071 * 0.7;
        vec2 iv = vec2 (1.0 / uAspect, 1.0);
        a *= iv;
        b *= iv;
        vec2 a2 = vec2 (-a.y / uAspect, a.x * uAspect);
        vec2 b2 = vec2 (-b.y / uAspect, b.x * uAspect);
        bodyW += cfAt (vUv + a).y + cfAt (vUv - a).y + cfAt (vUv + a2).y + cfAt (vUv - a2).y;
        bodyW += cfAt (vUv + b).y + cfAt (vUv - b).y + cfAt (vUv + b2).y + cfAt (vUv - b2).y;
        bodyW *= 0.125;
    }
    // (with a very soft silhouette, so the eyes and the maw, holes in the density, stay in its shadow;
    // only where there is body round it: spread out, the thin parts (spines, horns, the crown) would
    // lay a fan of straight-sided rays and a soft trapezoid over the head)
    float bodyS = smoothstep (0.12, 0.65, bodyW);
    float silS = (1.0 - smoothstep (-0.10, 0.10, d)) * smoothstep (0.15, 0.50, bodyW);
    float shadowM = clamp (0.4 * bodyS + 0.6 * silS, 0.0, 1.0) * smoothstep (0.02, 0.5, F) * (1.0 - 0.7 * gl) * (1.0 - 0.45 * uSleep);
    shadowM = max (shadowM, sleepHump (p) * keep);
    // the egg reads as a soft darker shape in the mist, as the creature does
    shadowM = max (shadowM, 0.85 * (1.0 - smoothstep (-0.08, 0.10, d)) * smoothstep (0.1, 0.5, egg));
    float softMaw = (1.0 - smoothstep (-0.015, 0.04, dMouth)) * Fm * keep;
    vec3 mf = mistField (p, 0.05 * vec2 (D.g - 0.5, D.r - 0.5));
    float mistV = mistValue (mf, p.y, clamp (D.r, 0.0, 1.2), clamp (sun, 0.0, 1.2), mistEarly() * (1.0 - 0.8 * shadowM) * (1.0 - 0.35 * uSleep)) + mistLate() - 0.28 * shadowM - 0.10 * softMaw;
    vec3 mistCol = atmoMistCol (mistV, mf.z);
    // its opacity: eased (uAtmo.x), a little uneven, a little thinner round the eyes, over the maw (so
    // the teeth read against its dark inside) and where the scene is deepest; never clear
    vec2 eRelM = (h - ec) / 0.14;
    float nearEyes = exp (-dot (eRelM, eRelM)) * smoothstep (0.0, 0.25, eyeGlow);
    float aM = uAtmo.x * (1.0 + 0.05 * mf.x);
    aM -= 0.05 * nearEyes + 0.03 * clamp (Lt.b, 0.0, 1.0) + 0.20 * softMaw;
    aM -= 0.08 * (1.0 - smoothstep (0.04, 0.16, lumS));
    float mistOn = step (0.5, uAtmo.w);
    aM = clamp (aM, 0.45, 0.97) * mistOn;
    col = mix (col, mistCol, aM);
    // the warm light veiled, not removed: the throat's and the eyes' glow tint the mist they light
    // (their hue only, a small share; the value stays the mist's)
    col = warmVeil (col, throatCol, 0.75 * clamp (1.4 * mawCore + 0.5 * clamp (Lt.a, 0.0, 1.0) * (1.0 - softMaw), 0.0, 1.0) * awake * mistOn);
    col = warmVeil (col, eyeCol, 0.35 * (0.8 * eyeM + 0.4 * nearEyes) * awake * mistOn);
    // the glows scattered in the mist (screen blend: soft halos, kept out of the maw's cavity)
    vec3 halo = bloomC * 0.14 * mix (vec3 (1.0), vec3 (0.9, 0.95, 1.0), 0.3) * (1.0 - 0.85 * (1.0 - smoothstep (-0.015, 0.10, dMouth)) * Fm * keep);
    col = 1.0 - (1.0 - col) * (1.0 - clamp (halo, 0.0, 1.0));

    vec2 vg = p * vec2 (0.42, 1.25);
    col *= 1.0 - 0.10 * dot (vg, vg);
    float luma = dot (col, vec3 (0.3, 0.59, 0.11));
    float gr = hash12 (floor (frag) + uGrain.xy) - 0.5;
    col += gr * (0.006 + 0.004 * (1.0 - luma) + (0.004 * uBoost + 0.002 * uDetailLow - 0.003 * uCut) * ab * F) * uGrain.z;
    col += outDither (frag);
    col = clamp (col, 0.0, 1.0);

    float rad = uRadius * uOutRes.y / max (uRes.y, 1.0);
    vec2 cq2 = abs (frag - 0.5 * uOutRes) - (0.5 * uOutRes - vec2 (rad));
    float dc = length (max (cq2, 0.0)) - rad;
    col = mix (col, uColOuter, smoothstep (-0.75, 0.75, dc));
    gl_FragColor = vec4 (col, 1.0);
}
)GLSL";

const char* const c_q0Pre = R"GLSL(
varying vec2 vUv;

uniform sampler2D uNoise;   // the baked noise: the top mist reads the same field as the fluid tiers
)GLSL";

const char* const c_q0Main = R"GLSL(
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
    // a stateless twirl round the vortices (the fluid tiers carry real, accumulating swirl)
    vec2 pt = p;
    for (int i = 0; i < 6; i++)
    {
        vec4 vt = uVort[i];
        vec2 rv = pt - vt.xy;
        float th = clamp (vt.w * 11.0, -2.2, 2.2) * exp (-dot (rv, rv) / max (vt.z * vt.z * 2.2, 1.0e-4));
        pt = vt.xy + rot (th) * rv;
    }
    vec2 cw = pt * 1.55 + uSeed.zw + uDrift.xy;
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
)GLSL";

const char* const c_q0Occ = R"GLSL(
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
)GLSL";

const char* const c_q0H = R"GLSL(

    // ---- height field for wet-ink shading (normals from screen derivatives)
    float H = 0.55 * (1.0 - exp (-max (-dBody, 0.0) * 9.0)) + 0.35 * (1.0 - exp (-max (-dHead, 0.0) * 5.0));
    H += (0.38 * smoothstep (0.02, -0.015, dBrow) + 0.22 * smoothstep (0.015, -0.01, dBrow2)) * browW;
    H += (0.18 * smoothstep (0.02, -0.01, dCheekBone) + 0.15 * smoothstep (0.02, -0.01, dNose)) * Fm;
    // the muzzle of the long-faced forms
    H += 0.28 * snout * smoothstep (0.03, -0.04, dMuzzle) * Fh;
    H -= 0.25 * smoothstep (0.006, -0.004, dNostril) * Fm;
    // wing fingers and the central figure stand up from the ink
    H += 0.12 * uEvoWing.w * smoothstep (0.035, -0.025, dSpur) * Fx;
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
    H -= 0.32 * smoothstep (0.02, -0.02, dSocket) * Fm;
    if (openX.x + openX.y + openX.z > 0.001)
    {
        H -= 0.20 * crease.x * exp (-dot ((h - cB) / vec2 (0.045, 0.025), (h - cB) / vec2 (0.045, 0.025))) * Fm;
        H -= 0.20 * crease.y * exp (-dot ((h - cK) / vec2 (0.040, 0.022), (h - cK) / vec2 (0.040, 0.022))) * Fm;
        H -= 0.20 * crease.z * exp (-dot ((h - cC) / vec2 (0.022, 0.050), (h - cC) / vec2 (0.022, 0.050))) * Fm;
    }
    H += 0.12 * smoothstep (0.015, -0.01, dLips) * Fm;
    H *= 1.0 - 0.6 * gl;
)GLSL";

const char* const c_q0Rest = R"GLSL(
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
    // (under the top mist the graph paper is all but gone: a crisp grid would read through it as lines)
    col = mix (col, uColGrid * vig, grid * 0.45 * (1.0 - smoothstep (0.0, 0.6, dens)) * (1.0 - 0.8 * step (0.5, uAtmo.w)));

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
    // (not under the top mist: drawn at half resolution and upscaled, they read as pale specks in it)
    inkCol += uColDetail * speck * (0.10 + 0.4 * uBoost) * smoothstep (0.0, -0.02, dInk) * solid * (1.0 - step (0.5, uAtmo.w));
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
    // the pupil: a slit across the almond that narrows to points at the lids, a dark soft core;
    // the catch-light is a small round spot off to one side, under it (never a bar across it)
    float ey = (h.y - ec.y) / max (eh * 1.1, 1.0e-3);
    float sw = (0.0035 + 0.0045 * (1.0 - heat) + 0.003 * sigma) * sqrt (clamp (1.0 - ey * ey, 0.0, 1.0));
    float slit = 1.0 - smoothstep (0.3 * sw, sw + 0.002, abs (h.x - ec.x));
    vec3 eyeFill = eyeCol * (0.35 + 1.2 * iris) * eI;
    vec2 clv = e - vec2 (-0.38 * ew, 0.3 * eh);
    eyeFill += vec3 (1.0, 0.95, 0.8) * exp (-dot (clv, clv) / 3.0e-5) * 0.5 * eI * eyeOpen;
    eyeFill = mix (eyeFill, vec3 (0.015, 0.0, 0.004), slit * eyeOpen * (0.82 + 0.13 * (1.0 - sigma)));
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
    // a cavity, not a lit dome: the glow sits low and back, the roof and the corners in shadow
    // (soft-clipped, so even a full roar keeps a falloff from the back of the throat to the lips)
    float yRel = clamp ((h.y - yL) / max (yU - yL, 1.0e-3), 0.0, 1.0);
    float core = exp (-(h.x * h.x) / (mw * mw * 0.18) - (yRel - 0.36) * (yRel - 0.36) / 0.09);
    float cavity = (1.0 - 0.75 * smoothstep (0.45, 1.0, yRel)) * (1.0 - 0.65 * smoothstep (0.3, 0.95, h.x / max (mw, 1.0e-3)));
    vec3 throat = throatFloor + throatCol * 0.6 * (1.0 - exp (-(0.3 * tg + 0.9 * core) * throatI * cavity / 0.6));
    float lipShade = smoothstep (0.0, 0.01 + 0.012 * sigma, min (yU - h.y, h.y - yL));
    throat = mix (mix (bodyCol, throatFloor, 0.5), throat, 0.3 + 0.7 * lipShade);
    throat += uColGrowl * (1.0 - lipShade) * uGrowl * abs (uGrowlMod) * 0.6 * awake;
    throat *= 1.0 - 0.6 * uOvReplace * ovA;   // Replace: the throat goes hollow
    col = mix (col, throat, mouth);
    // before the mouth has a shape, its glow is already there in the cloud
    col += throatCol * exp (-max (dMouth, 0.0) / 0.03) * throatI * 0.35 * Fe * (1.0 - Fm) * keep;

    // teeth: two rows behind, the main rows on the lips (they interlock when the jaw is shut);
    // translucent at the tips, a little of the cloud's colour in the bone
    float tremor = 0.0;   // (the growl's shiver is on the lips themselves now)
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
    float toothCov = backTeeth * teethF * Fm * 0.6;   // (for the maw's cavity under the grade)
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
        toothCov = max (toothCov, needles * teethF * 0.85);
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
    toothCov = max (toothCov, lowA);
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
    toothCov = max (toothCov, upA);
    // de-ess: a cold glint runs along the teeth on each S
    col += uColDeess * upA * pow (tc1, 3.0) * (0.4 + 0.6 * upShade) * deessGlint;

    // ring: a muzzle of dark metal bands over the mouth, flickering at a slowed copy of the carrier
    // (as in the fluid tiers: the skin's own value darkened, in a feathered oval, never a box)
    if (uRing > 0.001)
    {
        vec3 mz = ringMuzzle (h, mw, mY, open, uRingBody, 0.012 + 0.016 * sigma);
        float slats = 3.0 + 2.0 * uRingBody;
        float sv = fract (mz.y * slats);
        float slat = smoothstep (0.04, 0.2, sv) * smoothstep (0.96, 0.8, sv);
        float flk = 0.55 + 0.45 * uRingFlicker;
        vec3 metal = col * (0.5 + 0.2 * (1.0 - sv)) * mix (vec3 (1.0), uColRing, 0.15);
        metal += uColRing * pow (1.0 - abs (sv - 0.3) * 2.0, 8.0) * 0.06 * flk;   // a faint line of light on each band
        metal += uColRing * 0.05 * flk * (1.0 - sv);
        float rivet = 0.0;
        for (int i = 0; i < 3; i++)
            rivet += ringRivet (h, mw, mY, open, uRingBody, float (i)) * uRingBody;
        vec3 gap = col * 0.35 + uColRing * 0.04 * uRingFlicker;
        vec3 plateCol = mix (gap, metal, slat) + vec3 (0.25) * rivet;
        float plateA = min (clamp (0.3 + 1.4 * uRing, 0.0, 1.0), 0.85) * smoothstep (0.0, 0.03, uRing) * Fm * keep;
        col = mix (col, plateCol, mz.x * plateA);
        // its flicker, a soft glow just outside the outline
        col += uColRing * exp (-max (mz.z, 0.0) / 0.02) * (1.0 - mz.x) * uRing * 0.08 * flk * Fm * keep;
    }

    // rings and embers also play faintly over the creature
    col += uColSub * rings * subAmt * 0.18 * ink;
    col += uColRoar * embers * heat * 0.8 * awake * ink;

    // bioluminescent dots along the wing edges and the spines
    if (uEvoPal.w > 0.001)
    {
        // (jittered, sparse, sized and clustered by noise, as in the fluid tiers: never a lattice)
        vec2 bc = wq * 26.0;
        vec2 bcell = floor (bc);
        float bh = hash12 (bcell + 31.7);
        vec2 bo = vec2 (hash12 (bcell + 5.3), hash12 (bcell + 9.1)) * 0.7 + 0.15;
        float br = 0.06 + 0.08 * hash12 (bcell + 2.9);
        float bdd = length (fract (bc) - bo);
        float zoneB = smoothstep (0.42, 0.72, vnoise (wq * 3.1 + vec2 (1.7, 4.2)));
        float along2 = exp (-abs (dEdge + 0.012) / 0.02) + 0.3 * smoothstep (0.02, -0.005, dCrown);
        float dot1 = step (0.80, bh) * (1.0 - smoothstep (0.3 * br, br, bdd)) * along2 * zoneB * (0.4 + 0.6 * hash12 (bcell + 6.6));
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

    // ---- the grade and the top mist, as in the fluid tiers: the same mist field, the maw a cavity, the
    // body's shadow in the mist from its distance (this pass has no blurred density: the distance,
    // widened, stands in for it)
    if (uAtmo.w > 0.5)
    {
        float lumS = dot (col, vec3 (0.3, 0.59, 0.11));
        col = atmoGrade (col, lumS);
        float mawCore = clamp (core * throatI * cavity, 0.0, 1.0) * mouth;
        col = mix (col, atmoTone (0.10 + 0.22 * mawCore), mouth * (1.0 - toothCov));

        float bodyW = (1.0 - smoothstep (-0.16, 0.08, dEdge)) * (0.55 + 0.45 * ink);
        float silS = 1.0 - smoothstep (-0.10, 0.10, dEdge);
        float shadowM = clamp (0.4 * bodyW + 0.6 * silS, 0.0, 1.0) * smoothstep (0.02, 0.5, F) * (1.0 - 0.7 * gl) * (1.0 - 0.45 * uSleep);
        shadowM = max (shadowM, sleepHump (p) * keep);
        shadowM = max (shadowM, 0.85 * (1.0 - smoothstep (-0.08, 0.10, dEdge)) * smoothstep (0.1, 0.5, egg));
        float softMaw = (1.0 - smoothstep (-0.015, 0.04, dMouth)) * Fm * keep;
        vec3 mf = mistField (p, 0.05 * (warp - 0.5));
        // (+0.03: its body-shadow from the widened distance covers more of the wings than the blurred
        // density does, and this tier would sit a step darker than the others)
        float mistV = mistValue (mf, p.y, cloud, 0.6, mistEarly() * (1.0 - 0.8 * shadowM) * (1.0 - 0.35 * uSleep)) + mistLate() + 0.03 - 0.28 * shadowM - 0.10 * softMaw;
        vec3 mistCol = atmoMistCol (mistV, mf.z);
        vec2 eRelM = (h - ec) / 0.14;
        float eyeGlowM = clamp (eI * 0.5, 0.0, 1.0) * Fe * keep;
        float nearEyes = exp (-dot (eRelM, eRelM)) * smoothstep (0.0, 0.25, eyeGlowM);
        float throatL = clamp (tg * throatI * Fm * keep, 0.0, 1.0);
        float aM = uAtmo.x * (1.0 + 0.05 * mf.x);
        aM -= 0.05 * nearEyes + 0.03 * clamp (eyeHalo * eI, 0.0, 1.0) + 0.20 * softMaw;
        aM -= 0.08 * (1.0 - smoothstep (0.04, 0.16, lumS));
        aM = clamp (aM, 0.45, 0.97);
        col = mix (col, mistCol, aM);
        col = warmVeil (col, throatCol, 0.75 * clamp (1.4 * mawCore + 0.5 * throatL * (1.0 - softMaw), 0.0, 1.0) * awake);
        col = warmVeil (col, eyeCol, 0.35 * (0.8 * eye + 0.4 * nearEyes) * awake);
        // (no bloom in this pass: the eyes' light scattered in the mist is a soft halo of its own)
        vec3 haloM = eyeCol * eI * 0.06 * exp (-max (dEye, 0.0) / 0.035) * Fe * keep;
        col = 1.0 - (1.0 - col) * (1.0 - clamp (haloM * mix (vec3 (1.0), vec3 (0.9, 0.95, 1.0), 0.3), 0.0, 1.0));
        vec2 vgM = p * vec2 (0.42, 1.25);
        col *= 1.0 - 0.10 * dot (vgM, vgM);
    }
    col += outDither (frag);
    col = clamp (col, 0.0, 1.0);

    // rounded corners, filled with what is around the view
    vec2 cq2 = abs (frag - 0.5 * uRes) - (0.5 * uRes - vec2 (uRadius));
    float dc = length (max (cq2, 0.0)) - uRadius;
    col = mix (col, uColOuter, smoothstep (-0.75, 0.75, dc));

    gl_FragColor = vec4 (col, 1.0);
}
)GLSL";

// ---- The ink creature: one block shared by every program that draws or steps it, so the tiers
// cannot drift apart. A seeded, mirrored anatomy carried by an analytic flow that is odd across the
// fold; the ink may fill the anatomy as the flow carried it. Nothing here draws an eye, a tooth or
// any part of a face.
const char* const c_inkCommon = R"GLSL(
uniform vec2 uRes;
uniform float uPxScale;
uniform float uRadius;
uniform vec3 uColOuter;
uniform vec4 uMood;      // the colour mood: the palette's hue turn on the YIQ chroma plane, how warm it is
uniform vec3 uAtmoTint;  // the stage's hue lean (near white)
uniform sampler2D uNoise;   // R value, GB its gradient (0.5 = none), A ridged
uniform vec4 uBlob[8];   // lobes: centre xy, radii
uniform vec4 uBlobR[8];  // ... cos, sin of the turn, weight, core
uniform vec4 uLimbA[6];  // limbs: root xy, bend xy
uniform vec4 uLimbB[6];  // ... tip xy, root radius, tip radius
uniform vec4 uMaw;       // the rift: height on the fold, half width, opening, strand amount
uniform vec4 uBay[2];    // bays: centre xy, radius, direction
uniform vec4 uFlowK;     // eddy gain, small-eddy share, bound pair strength, 1 / its radius squared
uniform vec4 uFlowO;     // where the large eddies are read xy, the small ones xy
uniform vec4 uFlowC;     // the bound pair's centre xy, the strands' gap, the body's centre height
uniform vec4 uCarry;     // carry time formed, extra carry unformed, how unformed (0 formed .. 1 cloud), the smoke's further carry
uniform vec4 uInkF;      // formation, roar (capacity), feather, density
uniform vec4 uInkO;      // the blot's noise offset xy, a slow scroll, ribbon
uniform vec4 uCloudK;    // mirror share, bank opacity, gap opacity, roar thinning
uniform vec4 uCloudO;    // far layer drift xy, near layer drift xy
uniform vec4 uBreath;    // the rift's light now, the breath's jet, its rise (a scroll), level
uniform vec4 uCloudT;    // the far cloud's two carries (seconds), the second's share, the flow at rest over the flow now

// The coarsest level of the noise a lookup of this many noise tiles per view height may read at
// this size of target without skipping texels (the pattern is the same at any size, never grit).
#define PXL(s) (log2 ((s) * 256.0 / uRes.y) + 0.5)

float hash12 (vec2 p)
{
    vec3 p3 = fract (vec3 (p.xyx) * 0.1031);
    p3 += dot (p3, p3.yzx + 33.33);
    return fract ((p3.x + p3.y) * p3.z);
}

// the noise's value stretched to use most of 0..1, and its gradient per unit of noise coordinate
float sv (vec4 n) { return (n.r - 0.5) * 1.45 + 0.5; }
vec2 gv (vec4 n) { return (n.gb - vec2 (0.5)) * 46.4; }

// The flow at a point of the half plane (x >= 0), view heights per second. Its stream function is
// odd across the fold, so the fold is a streamline: the large eddies are the noise minus its mirror
// image, the small ones fade out at the fold, and one mirrored pair of vortices is bound to the body.
vec2 flow (vec2 m)
{
    vec4 a = NZ (m * 0.19 + uFlowO.xy, 2.5);
    vec4 a2 = NZ (vec2 (-m.x, m.y) * 0.19 + uFlowO.xy, 2.5);
    vec2 ga = (a.gb - vec2 (0.5)) * 32.0, ga2 = (a2.gb - vec2 (0.5)) * 32.0;
    vec2 g = 0.19 * vec2 (ga.x + ga2.x, ga.y - ga2.y);
    vec4 b = NZ (m * 0.47 + uFlowO.zw, 1.5);
    vec2 gb = (b.gb - vec2 (0.5)) * 32.0;
    float s = clamp (m.x / 0.25, 0.0, 1.0);
    float r = s * s * (3.0 - 2.0 * s), dr = 6.0 * s * (1.0 - s) / 0.25;
    g += uFlowK.y * 0.55 * vec2 (r * gb.x * 0.47 + dr * (b.r - 0.5), r * gb.y * 0.47);
    vec2 v = vec2 (g.y, -g.x) * uFlowK.x;
    vec2 d1 = m - uFlowC.xy, d2 = vec2 (m.x + uFlowC.x, m.y - uFlowC.y);
    float e1 = exp (-dot (d1, d1) * uFlowK.w), e2 = exp (-dot (d2, d2) * uFlowK.w);
    v += uFlowK.z * vec2 (-d1.y * e1 + d2.y * e2, d1.x * e1 - d2.x * e2);
    // the ceiling: nothing carries ink faster than 0.6 view heights a second
    return v * inversesqrt (1.0 + dot (v, v) / 0.36);
}

// Where the ink at m was tau seconds ago (four backward steps through the flow).
vec2 backtrace (vec2 m, float tau)
{
    vec2 q = m;
    float h = tau * 0.25;
    for (int i = 0; i < 4; ++i)
    {
        q -= flow (q) * h;
        q.x = abs (q.x);
    }
    return q;
}

float smin (float a, float b, float k) { float h = max (k - abs (a - b), 0.0) / k; return min (a, b) - h * h * k * 0.25; }
float smax (float a, float b, float k) { return -smin (-a, -b, k); }
float sdSeg (vec2 p, vec2 a, vec2 b, float ra, float rb)
{
    vec2 pa = p - a, ba = b - a;
    float h = clamp (dot (pa, ba) / max (dot (ba, ba), 1.0e-6), 0.0, 1.0);
    return length (pa - ba * h) - mix (ra, rb, h);
}

// The rift at a point of the half plane: negative in the open tear, positive in the ink round it
// and on the strands left across it. (gRift: the tear alone; gStrand: positive on a strand.)
float gRift, gStrand;
float riftField (vec2 q)
{
    float open = uMaw.z;
    // the rift: a tear on the fold, taller than wide when open and shut when the jaw is; its outline
    // is eroded by noise that travels with the ink
    vec2 mc = vec2 (q.x, q.y - (uMaw.x - open * 0.15));
    vec2 mr = vec2 (uMaw.y * (0.6 + 1.1 * smoothstep (0.0, 0.12, open)), 0.004 + open * 0.75);
    // (a crack, not an oval: pointed at both ends, its width uneven along its length)
    float ty = clamp (mc.y / mr.y, -1.0, 1.0);
    float uneven = clamp (sv (NZ (vec2 (0.61, q.y * 1.3) + uInkO.xy, 1.0)), 0.0, 1.0);
    // (its edge is torn by noise that travels with the ink, but only ever narrows or widens the one
    // opening on the fold: an opening torn off to one side would be mirrored into a pair)
    float torn = clamp (sv (NZ (q * 4.5 + uInkO.yx, max (0.5, PXL (4.5)))), 0.0, 1.0);
    float rw = mr.x * pow (1.0 - ty * ty, 0.75) * (0.35 + 1.5 * uneven) * (0.55 + 0.9 * torn);
    float rift = max (abs (mc.x) - rw, abs (mc.y) - mr.y) + (1.0 - smoothstep (0.0, 0.02, open)) * 0.25;
    // strands of ink left across the rift, thinning as it opens: unevenly spaced, leaning and
    // wandering, each swelling and pinching along its length (stretched ink, never a row of pegs)
    float sy = q.y - uMaw.x;
    float sx = q.x * (1.0 + 1.6 * sy) + (NZ (vec2 (q.y * 1.3, 0.37) + uInkO.xy, 1.0).r - 0.5) * 0.09;
    // (close enough that at least a pair of them crosses the rift wherever it is wide)
    float gap = max (0.01, mr.x * (0.20 + 0.55 * uFlowC.z));
    float su = sx / gap;
    su += 0.33 * sin (su * 2.1 + uInkO.x * 50.0);
    float swell = NZ (vec2 (floor (su) * 0.173 + uInkO.y, q.y * 2.3 + uInkO.x), max (0.0, PXL (2.3))).r;
    float thread = (0.0035 + (0.004 + 0.008 * uMaw.w) * smoothstep (0.25, 0.75, swell)) * (1.0 - open * 3.2) - abs (fract (su) - 0.5) * gap;
    gRift = rift;
    gStrand = thread;
    return max (rift, thread);
}

// The seeded anatomy in the half plane: lobes, bent tapered limbs, the rift on the fold, bays.
float anatomySdf (vec2 q)
{
    float open = uMaw.z;
    // the weighted jaw: everything under the rift hangs lower (the ink between stretches), and what
    // is above it lifts by a third as much, so the blot opens about its middle and stays in view
    float below = 1.0 - smoothstep (uMaw.x - 0.14, uMaw.x + 0.03, q.y);
    vec2 p = vec2 (q.x, q.y + open * (below - 0.35));
    float d = 9.0;
    for (int i = 0; i < 8; ++i)
    {
        vec4 b = uBlob[i];
        vec4 r = uBlobR[i];
        vec2 l = p - b.xy;
        l = vec2 (r.x * l.x + r.y * l.y, -r.y * l.x + r.x * l.y);
        vec2 rad = b.zw * r.z + 1.0e-4;
        float e = (length (l / rad) - 1.0) * min (rad.x, rad.y) + (1.0 - r.z) * 0.6;
        d = smin (d, e, 0.055);
    }
    for (int i = 0; i < 6; ++i)
    {
        vec4 A = uLimbA[i];
        vec4 B = uLimbB[i];
        float rm = mix (B.z, B.w, 0.45);
        float e = min (sdSeg (p, A.xy, A.zw, B.z, rm), sdSeg (p, A.zw, B.xy, rm, B.w));
        d = smin (d, e, 0.045);
    }
    d = smax (d, -riftField (q), 0.03);
    // the bays: tapered cuts that widen towards the outside, so the cloud they let in is never enclosed
    for (int i = 0; i < 2; ++i)
    {
        vec4 b = uBay[i];
        float e = sdSeg (p, b.xy, b.xy + 1.5 * vec2 (cos (b.w), sin (b.w)), b.z, b.z * 3.0);
        d = smax (d, -e, 0.03);
    }
    return d;
}

// What inkTarget leaves behind for the passes that draw.
vec2 gCarried;      // where this point's ink came from
float gStretch;     // how many levels coarser the carried frame must be read (the carry stretches it)
float gHaloD;       // the unfeathered distance, eroded while unformed
float gFringe;      // the same field carried further, wider and more eroded (smoke)

// The shape the ink may fill at a point of the (soft) half plane: the anatomy as the flow carried
// it. 0.5 on the edge, more inside. Forming and dissolving never scale an opacity: the carry gets
// longer and the shape erodes in patches, so the ink arrives and leaves as shreds.
float inkTarget (vec2 m)
{
    float un = uCarry.z;
    vec4 nA = NZ (m * 0.7 + uInkO.xy * 1.7 + vec2 (0.0, uInkO.z), 1.5);
    float patch = clamp (sv (nA), 0.0, 1.0);
    // a short, ever-changing carry while formed (longer in patches: ribbons); a long one unformed
    float tau = uCarry.x * (1.0 + 2.2 * uInkO.w * smoothstep (0.45, 0.85, patch)) + uCarry.y * (9.0 + 14.0 * patch);
    // near the rift the carry is weak: the rift stays where the jaw puts it
    float ry = clamp (m.y, uMaw.x - 0.90 * uMaw.z, uMaw.x + 0.60 * uMaw.z);
    float nearRift = 1.0 - smoothstep (0.0, 0.12, length (vec2 (m.x, m.y - ry)));
    tau *= 1.0 - 0.7 * nearRift * (1.0 - un) * (1.0 - un);
    vec2 q = backtrace (m, tau);
    // unformed, each patch is also drawn aside its own way: the shreds gather from all round
    q += un * 2.2 * (nA.gb - vec2 (0.5));
    q.x = abs (q.x);
    gCarried = q;
    // (where the carry has stretched the frame, its detail is read coarser: no combed lines)
    gStretch = log2 (max (1.0, max (length (dFdx (q)), length (dFdy (q))) * uRes.y));
    float d0 = anatomySdf (q);
    // feathering that travels with the ink (read in the carried frame)
    vec4 n1 = NZ (q * 0.6 + uInkO.xy, 1.0 + gStretch);
    vec4 n2 = NZ (q * 2.7 - uInkO.xy, max (0.0, PXL (3.8)) + gStretch);
    float feather = ((sv (n1) - 0.5) * (0.09 + 0.10 * un) + (sv (n2) - 0.5) * 0.03) * uInkF.z * (1.0 - 0.6 * nearRift);
    // erosion while unformed: the outside goes first, in patches the noise picks
    // (the floor takes what is thin first, everywhere: the limbs go before the core)
    float erosion = un * (0.10 + 1.4 * patch * patch) * (0.4 + un) + smoothstep (0.65, 1.0, un) * 0.5;
    float d = d0 + feather + erosion;
    gHaloD = d0 + erosion;
    // the smoke fringe: the same anatomy carried further down the flow (two more steps)
    vec2 q2 = q - flow (q) * uCarry.w;
    q2.x = abs (q2.x);
    q2 -= flow (q2) * uCarry.w;
    q2.x = abs (q2.x);
    float dF = anatomySdf (q2) + (sv (n1) - 0.5) * 0.22 * uInkF.z + (patch - 0.5) * 0.16 + erosion * 1.3 - 0.035;
    gFringe = clamp (0.5 - dF / 0.25, 0.0, 1.5);
    return clamp (0.5 - d / 0.20, 0.0, 1.5) * (1.0 + 0.4 * uInkF.y);
}

// ---- palette: every colour is built from one tone, turned by the mood
vec3 moodTurnBy (vec3 c, float k)
{
    vec2 iq = vec2 (dot (c, vec3 (0.596, -0.274, -0.322)), dot (c, vec3 (0.211, -0.523, 0.312)));
    vec2 d = k * vec2 (uMood.x * iq.x - uMood.y * iq.y, uMood.y * iq.x + uMood.x * iq.y);
    return c + d.x * vec3 (0.956, -0.272, -1.106) + d.y * vec3 (0.621, -0.647, 1.703);
}

// One lit layer of cloud at a point of the view, carried by d: a large octave (its own picture,
// part of it folded to its mirror image) and a finer one that takes less of the carry, so large
// shapes turn and puffs stay puffs. x: how dense, about 0.5; yz: its slope (lit from it).
vec3 cloudLayer (vec2 p, vec2 d, float share, float fa, float fb, vec2 o, float lodA, float lodB)
{
    vec4 a = NZ ((p - d) * fa + o, lodA);
    vec4 am = NZ ((vec2 (-p.x, p.y) - vec2 (-d.x, d.y)) * fa + o, lodA);
    am.g = 1.0 - am.g;
    a = mix (a, am, share);
    vec4 b = NZ ((p - d * 0.3) * fb + a.r * 0.08 - o * 0.7, lodB);
    return vec3 (sv (a) * 0.66 + sv (b) * 0.34, gv (a) * fa + gv (b) * fb * 0.30);
}

// About one 8-bit step of triangular dither: the pale, smooth cloud bands on an 8-bit target without it
vec3 outDither (vec2 frag)
{
    float ign = fract (52.9829189 * fract (dot (frag, vec2 (0.06711056, 0.00583715))));
    return vec3 ((ign + hash12 (frag + 7.7) - 1.0) / 255.0);
}
)GLSL";

// ---- The ink buffer (the tiers with one): half the view (x >= 0), mirrored across the fold by its
// wrap mode. R held ink, G free pigment, B the marbling tone (signed), A the breath.
const char* const c_inkBuffer = R"GLSL(
uniform sampler2D uInkTex;
uniform vec4 uInkRes;    // the buffer's size, the half view's width in view heights, the step's time
uniform vec4 uInkStep;   // 1: fill the buffer afresh; 1: bytes (not half floats); a dither phase; spare
uniform vec4 uInkRule;   // the body's floor (0..1, long formed), how fast the hold is letting go (0..1), how much smoke ink let go makes, the gathering's share of the pull
uniform vec4 uHold;      // hold, release (per second), drift (+ inwards, - outwards; view heights a second), its reach
uniform vec4 uPig;       // the cloud's own pigment, the budget's gain on it, the release's curl kick, sink
uniform vec4 uInkTier;   // cloud layers (2 or 3), 1: the far cloud is carried in two phases, 1: threads, spare
uniform vec4 uSubPulse;  // the sub's push: its centre's height, speed outwards (view heights a second), reach, spare
uniform vec4 uSparkP[3]; // beads of pigment thrown from the rim: where xy, radius, pigment a second

vec4 inkDecode (vec4 s) { return uInkStep.y > 0.5 ? vec4 (s.r * 2.5, s.g * 2.5, s.b * 2.0 - 1.0, s.a * 2.0) : s; }
vec4 inkRead (vec2 m, float lod)
{
    return inkDecode (IT (vec2 (min (abs (m.x) / uInkRes.z, 1.0), m.y + 0.5), lod));
}
)GLSL";

// ---- One step of the ink. Nothing is drawn into the buffer as a shape and nothing is faded: free
// pigment that is already in the cloud is carried by the flow and drawn towards the body, and turns
// into held ink where it has arrived and the shape has room; held ink the shape no longer covers
// turns back into free pigment where it is, and from then on belongs to the flow.
const char* const c_inkSim = R"GLSL(
void main()
{
    vec2 uv = gl_FragCoord.xy / uInkRes.xy;
    vec2 m = vec2 (uv.x * uInkRes.z, uv.y - 0.5);
    vec2 ms = vec2 (sqrt (m.x * m.x + 0.0004), m.y);
    float dt = uInkRes.w, amb = uPig.x;
    float cap = inkTarget (ms);
    // where pigment turns into ink it takes the tone of the place: the body shows where its ink came from
    float tone = clamp ((sv (NZ (m * 0.33 + uInkO.yx * 1.3, 2.0)) - 0.5) * 3.0, -1.0, 1.0);
    vec4 o;
    if (uInkStep.x > 0.5)
        o = vec4 (cap, amb * (1.0 - smoothstep (0.2, 0.7, cap)), tone, 0.0);
    else
    {
        vec2 v = flow (m) * 2.5;
        // the pull: towards the body while it gathers (on through it, slower, to its middle), away
        // from it while it lets go
        float dA = anatomySdf (ms);
        vec2 gA = vec2 (dFdx (dA) * uInkRes.x / uInkRes.z, dFdy (dA) * uInkRes.y);
        vec2 dir = gA / (length (gA) + 1.0e-4);
        // (the pull eases off over a wide band as it reaches the body, so the pigment gathers as a
        // soft hull of smoke round where the body will be, never as its outline or its skeleton)
        float pull = uHold.z > 0.0 ? (1.0 - smoothstep (0.5 * uHold.w, uHold.w, dA)) * mix (0.3 * smoothstep (0.5, 0.9, uInkF.x), 1.0, smoothstep (-0.05, 0.25, dA))
                                   : 1.0 - smoothstep (0.0, 0.5, dA);
        // (no pull across the fold: pigment drawn onto it from both sides would pile into a line)
        vec2 drift = -dir * uHold.z * pull * vec2 (smoothstep (0.0, 0.12, m.x), 1.0);
        // the kick: small eddies that take only the free pigment (none of it crosses the fold)
        vec4 kn = NZ (m * 1.9 + uInkO.xy * 3.0 + vec2 (0.0, 2.0 * uInkO.z), max (0.5, PXL (1.9)));
        float e = clamp (m.x / 0.2, 0.0, 1.0);
        float ks = e * e * (3.0 - 2.0 * e), dks = 6.0 * e * (1.0 - e) / 0.2;
        vec2 kg = gv (kn) * 1.9;
        vec2 kick = uPig.z * 0.03 * vec2 (ks * kg.y, -(ks * kg.x + dks * (sv (kn) - 0.5)));
        vec4 kn2 = NZ (m * 4.3 - uInkO.xy * 5.0 + vec2 (3.0 * uInkO.z, 0.0), max (0.5, PXL (4.3)));
        vec2 kg2 = gv (kn2) * 4.3;
        kick += uPig.z * 0.008 * vec2 (ks * kg2.y, -(ks * kg2.x + dks * (sv (kn2) - 0.5)));
        // the breath: out of the rift, up and away from the fold
        vec2 rc = vec2 (m.x, m.y - uMaw.x);
        float jawOpen = smoothstep (0.015, 0.10, uMaw.z);
        vec2 breath = uBreath.y * 0.30 * jawOpen * exp (-dot (rc, rc) / 0.03) * normalize (vec2 (m.x * 4.0, 1.0));
        // the sub's push: outwards from the body's middle, once for each onset (none of it across the fold)
        vec2 rs = vec2 (m.x, m.y - uSubPulse.x);
        float rl = length (rs);
        vec2 push = uSubPulse.y * (rl / uSubPulse.z) * exp (0.5 - 0.5 * rl * rl / (uSubPulse.z * uSubPulse.z)) * rs / (rl + 1.0e-3);
        vec2 vG = v + drift + kick + breath + push - vec2 (0.0, uPig.w);
        // (drawn in, pigment gathers where the pull converges and the cloud pales where it spreads;
        // only the pull does this: the eddies carry pigment without ever piling it up)
        // (and only while it gathers: the slight pull that goes on awake carries, and piles nothing)
        float conv = uHold.z > 0.0 ? uInkRule.w * clamp (-(dFdx (drift.x) * uInkRes.x / uInkRes.z + dFdy (drift.y) * uInkRes.y) * dt, -0.08, 0.08) : 0.0;

        vec4 a = inkRead (m - (v + 0.5 * push) * dt, 0.0);
        vec2 mg = m - vG * dt;
        // (it spreads a little as it goes; more beside the fold, which no flow crosses and which
        // would otherwise keep a stripe of whatever it last held)
        float g = mix (inkRead (mg, 0.0).g, inkRead (mg, 2.5).g, 0.04 + 0.20 * (1.0 - smoothstep (0.0, 0.10, m.x)));
        // (what arrives is the cloud's own pigment at the pull's pace: it adds up, it does not multiply)
        g += conv * (conv > 0.0 ? min (g, 1.5 * amb) : g);
        // beads of pure pigment thrown from the rim by a clipped peak
        for (int i = 0; i < 3; ++i)
        {
            vec2 bd = m - uSparkP[i].xy;
            g += uSparkP[i].w * dt * exp (-dot (bd, bd) / (uSparkP[i].z * uSparkP[i].z));
        }
        float c = a.r, q = a.b;
        float b = inkRead (m - (v + breath) * dt, 0.0).a;

        // pigment condenses into ink (a little pigment is a lot of ink) and ink let go spreads into
        // smoke; while the hold lets go, ink leaves even where the shape still stands
        float take = min (uHold.x * (1.0 - uInkRule.y) * dt * max (cap - c, 0.0) / 3.0, g);
        // (let go in streaks, not all at once: some of the ink holds on a little longer than the rest)
        // (fine streaks, and none holds on for long: what is left is many shreds, never a pair of blots)
        float streak = NZ (m * 5.5 + uInkO.yx * 2.0 - v * 0.8, max (0.5, PXL (5.5))).a;
        float give = min (uHold.y * dt * max (c - cap, 0.0) + uInkRule.y * (0.9 + 1.5 * smoothstep (0.25, 0.75, streak)) * dt * c, c);
        q = (q * c + tone * 3.0 * take) / (c + 3.0 * take + 1.0e-4);
        c += 3.0 * take - give;
        // (awake, what the rim sheds is the pigment it was made of, a wisp; let go, the body's ink
        // spreads into heavy smoke)
        g += uInkRule.z * give - take;
        // (long formed, the body never empties, whatever is pulling at it and however little
        // pigment the cloud has left to give)
        c += max (0.85 * min (cap, 1.3) - c, 0.0) * uInkRule.x * min (1.0, 3.0 * dt);
        // away from the body the cloud comes back to its own pigment: slowly where it paled, sooner
        // where smoke hangs
        float away = 1.0 - smoothstep (0.0, 0.15, cap);
        float want = amb * uPig.y;
        // (smoke clears in patches, never evenly: the cloud opens holes in it)
        g += (want - g) * away * min (1.0, dt * (g > want ? 0.15 + 0.45 * smoothstep (0.35, 0.75, sv (kn)) : 0.04));
        // the breath: lit vapour fed low in the open rift, carried out, gone in a second
        float src = uBreath.x * jawOpen * (0.4 + 2.4 * uMaw.z) * smoothstep (0.25, 0.7, inkRead (m, 3.5).r) * exp (-(rc.x * rc.x) / (0.0009 + uMaw.y * uMaw.y) - (rc.y + 0.3 * uMaw.z) * (rc.y + 0.3 * uMaw.z) / (0.0009 + 0.3 * uMaw.z * uMaw.z));
        b = b * exp (-dt / 1.1) + 2.5 * dt * src;
        o = vec4 (c, g, q, b);
    }
    o = clamp (o, vec4 (0.0, 0.0, -1.0, 0.0), vec4 (2.5, 2.5, 1.0, 2.0));
    if (uInkStep.y > 0.5)
        o = vec4 (o.r * 0.4, o.g * 0.4, o.b * 0.5 + 0.5, o.a * 0.5) + (hash12 (gl_FragCoord.xy + uInkStep.z) - 0.5) / 255.0;
    gl_FragColor = o;
}
)GLSL";

// ---- The single pass: the whole creature without any buffer. The held ink is the target itself,
// per pixel; a halo and a smoke fringe stand in for the free pigment; two cloud layers.
const char* const c_inkSingle = R"GLSL(
void main()
{
    vec2 frag = gl_FragCoord.xy;
    vec2 p = (frag - 0.5 * uRes) / uRes.y;               // view heights, the centre at 0
    float sgn = p.x < 0.0 ? -1.0 : 1.0;
    vec2 m = vec2 (abs (p.x), p.y);
    vec2 ms = vec2 (sqrt (p.x * p.x + 0.0004), p.y);     // the soft fold: no crease down the middle
    vec2 v = flow (m);
    vec2 vw = vec2 (v.x * sgn, v.y);
    float F = uInkF.x, un = uCarry.z, roar = uInkF.y, open = uMaw.z;

#ifdef INK_BUFFER
    // the ink as the buffer holds it: held ink, the free pigment above (and below) what the cloud
    // always carries, the tone it took, the breath
    vec4 held = inkRead (m, 0.0);
    // (pigment the flow has wound dense is ink again, wherever it is: what the body lets go stays
    // ink until the eddies have thinned it)
    float inkFloor = 1.2 * uPig.x * max (1.0, uPig.y);
    float heldShare = held.r / (held.r + 1.1 * max (held.g - inkFloor, 0.0) + 1.0e-3);
    float body = held.r + 1.1 * max (held.g - inkFloor, 0.0);
    // (the buffer's ink lags the shape a little, which widens its edge: drawn steeper about the edge)
    body = max (0.5 + (body - 0.5) * 1.7, 0.0);
    vec4 wide = inkRead (m, 4.0);
    float halo = clamp (0.85 * (wide.r + 1.1 * max (wide.g - inkFloor, 0.0)), 0.0, 1.0);
    // the rift is drawn here, per pixel, from the jaw as it is now: the buffer's ink follows the jaw
    // a moment late and is too coarse for the strands. The ink thins away into the tear over a
    // width picked by fine noise that rises with the breath, a smoky edge and never a cut-out's;
    // the strands across it stay whole and sharp.
    vec4 rn = NZ (vec2 (m.x * 5.1, m.y * 3.7) + vec2 (0.0, -2.0 * uBreath.z) + uInkO.yx, max (0.5, PXL (5.1)));
    riftField (ms);
    // (only where the ink stands round it: no tear hangs in the open cloud)
    float tearSoft = (1.0 - smoothstep (-0.006 - 0.040 * clamp (sv (rn), 0.0, 1.0), 0.012, gRift)) * smoothstep (0.15, 0.35, halo);
    float hole = tearSoft * (1.0 - smoothstep (-0.0025, 0.0015, gStrand));
    // (the buffer's own late, coarse tear is filled in first: only this one is drawn)
    body = max (body, (1.0 - smoothstep (0.0, 0.035, gRift)) * smoothstep (0.3, 0.6, halo));
    float pigment = inkRead (m, 0.7).g;
    float loose = 2.0 * max (pigment - uPig.x * max (1.0, uPig.y), 0.0);
    float paled = smoothstep (0.15, 0.9, 1.0 - inkRead (m, 2.5).g / (uPig.x * min (1.0, uPig.y)));
    float breathLit = inkRead (m, 1.0).a, breathWide = inkRead (m, 3.5).a;
    gCarried = m - v * 2.0;
    gStretch = 0.0;
#else
    float body = inkTarget (ms);
    float halo = clamp (0.5 - gHaloD / 0.5, 0.0, 1.0);
#endif
#ifdef INK_BUFFER
    // (the sheen's slope from a coarser level of the buffer, by differences: the slope of one
    // magnified texel is constant across it, and would facet the gleam)
    float gt = 1.5 / uInkRes.y;
    vec2 grad = 1.7 * 0.0114 / (2.0 * gt) * vec2 (inkRead (vec2 (p.x + gt, p.y), 1.5).r - inkRead (vec2 (p.x - gt, p.y), 1.5).r,
                                                  inkRead (vec2 (p.x, p.y + gt), 1.5).r - inkRead (vec2 (p.x, p.y - gt), 1.5).r);
#else
    vec2 grad = vec2 (dFdx (body), dFdy (body)) * (uRes.y * 0.0114);
#endif

    // detail carried by the flow (a single phase, the carry capped)
    vec2 cap = v * 3.0;
    cap *= 0.1 / max (0.1, length (cap));
    vec2 capw = vec2 (cap.x * sgn, cap.y);
    vec4 det = NZ ((m - cap) * 0.9 + uInkO.xy, max (0.5, PXL (0.9)));
    vec4 fine = NZ (m * 1.7 + det.r * 0.04 + 0.3 - v * 1.5 + uInkO.yx, max (0.7, PXL (1.7)));
    float detV = sv (det), fineV = sv (fine);

    // the body's edge: a smooth field thresholded, so it is crisp at any size; the threshold is
    // feathered by the carried noise at the edge only (the deep ink stays whole)
    float edgeW = 1.0 - smoothstep (0.60, 0.95, body);
#ifdef INK_BUFFER
    // threads: ridged noise read where the flow would have carried it from, seconds back. The
    // carry winds it along the eddies, so loose pigment hangs in filaments and curtains and the
    // ink's edge frays into ribbons.
    float thr = 0.5;
    if (uInkTier.z > 0.5)
    {
        float hT = 4.0 * uCloudT.w;
        vec2 qT = m - v * hT;
        qT.x = abs (qT.x);
        qT -= flow (qT) * hT;
        qT.x = abs (qT.x);
        float st = log2 (max (1.0, max (length (dFdx (qT)), length (dFdy (qT))) * uRes.y));
        thr = NZ (qT * 2.6 + uInkO.yx * 3.0, max (0.5, PXL (2.6)) + st).a;
    }
    float f = body + edgeW * ((detV - 0.5) * 0.24 + (fineV - 0.5) * 0.12) + (1.0 - smoothstep (0.7, 1.2, body)) * (thr - 0.5) * 0.24;
#else
    float f = body + edgeW * ((detV - 0.5) * 0.24 + (fineV - 0.5) * 0.12);
#endif
    float soft = smoothstep (0.35, 0.75, fine.a * 0.6 + 0.5 * (1.0 - detV));
    // (never crisper than the target's own pixels: the edge is the same at half size)
    float aa = max (0.008 + 0.045 * soft * soft + 0.05 * un, 0.75 * (abs (dFdx (f)) + abs (dFdy (f))));
    float ink = smoothstep (0.5 - aa, 0.5 + aa, f);
    float thick = smoothstep (0.5, 0.85, f);
    float rim = ink * (1.0 - smoothstep (0.5, 0.60, f));            // pigment pooled at the edge
    float veil = smoothstep (0.22, 0.52, f) * (1.0 - ink);          // the wet bleed outside the edge
#ifdef INK_BUFFER
    ink *= 1.0 - hole;
    thick *= 1.0 - hole;
    rim *= 1.0 - hole;
    veil *= 1.0 - hole;
#endif
#ifdef INK_BUFFER
    // free pigment: smoke where it is thin, ink again where the flow has wound it dense; threaded
    // by the carried noise, so it hangs in filaments rather than as a grey fog
    float thread = uInkTier.z > 0.5 ? 0.25 + 1.7 * smoothstep (0.15, 0.85, thr) : 0.75 + 0.5 * fine.a * (0.4 + detV);
    // (thin smoke hangs in threads; where the flow has wound it dense it is ink, whole)
    thread = mix (thread, max (thread, 1.0), smoothstep (0.25, 0.8, loose));
    float smoke = (1.2 * loose + 1.8 * loose * loose) * thread * (1.0 - 0.8 * ink);
    float smokeDense = smoothstep (0.3, 1.0, loose);
    // (smoke over the lit cloud: deep enough that ink let go is still dark while it is dense)
#define SMOKE_DEPTH 1.25
#else
#define SMOKE_DEPTH 0.9
    float fs = gFringe + (detV - 0.5) * 0.45 + (fineV - 0.5) * 0.2;
    float aaS = max (0.05, 0.75 * (abs (dFdx (fs)) + abs (dFdy (fs))));
    float smoke = smoothstep (0.52 - aaS, 0.52 + aaS, fs) * (1.0 - ink) * (0.45 + 1.1 * detV * (0.4 + fine.a));
    float smokeDense = 0.0;
#endif

    // ---- the cloud behind: lit billows, carried by the flow; its own picture asleep, folding
    // towards the blot's symmetry near the creature as it forms
    vec3 tone = moodTurnBy (vec3 (0.580, 0.690, 0.757) * uAtmoTint, 1.0);
    vec3 shadow = tone * 0.60, mid = tone * 1.04, light = mix (tone, vec3 (1.0), 0.86);
    vec2 L = normalize (vec2 (-0.45, 0.89));
    float share = mix (0.04, uCloudK.x, 1.0 - smoothstep (0.5, 1.3, length (p - vec2 (0.0, uFlowC.w))));
    vec3 farC = cloudLayer (p, capw * 0.6, share, 0.13, 0.34, uCloudO.xy, 3.5, 2.5);
#ifdef INK_BUFFER
    if (uInkTier.y > 0.5)
    {
        // two carries half a cycle apart, each faded out before it starts again: the far cloud is
        // wound by the eddies for seconds on end and never jumps back. (At the flow's rest pace:
        // the cloud turns with the eddies and does not shake with the voice.)
        vec2 vr = vw * uCloudT.w;
        vec2 dA = vr * uCloudT.x, dB = vr * uCloudT.y;
        dA *= 0.4 / max (0.4, length (dA));
        dB *= 0.4 / max (0.4, length (dB));
        float wB = uCloudT.z;
        farC = mix (cloudLayer (p, dA * 0.6, share, 0.13, 0.34, uCloudO.xy, 3.5, 2.5),
                    cloudLayer (p, dB * 0.6, share, 0.13, 0.34, uCloudO.xy, 3.5, 2.5), wB);
        // (two pictures mixed are flatter than one: its contrast is given back)
        float kc = inversesqrt (1.0 - 2.0 * wB * (1.0 - wB));
        farC = vec3 (0.5 + (farC.x - 0.5) * kc, farC.yz * kc);
    }
#endif
    vec4 c3 = NZ (p * 0.9 - capw * 0.3 + farC.x * 0.05 + uCloudO.xy, 1.5);
    float dens = farC.x * 0.94 + sv (c3) * 0.10;
    float lit = clamp (0.5 + dot (farC.yz, L) * 0.42, 0.0, 1.0);
    float cv = clamp (0.16 + 0.60 * lit + 0.95 * (dens - 0.5), 0.0, 1.0);
    cv = cv * cv * (3.0 - 2.0 * cv);
    // the halo: the cloud round the body is paler (its pigment was taken), the ink shades what is close
#ifdef INK_BUFFER
    cv = mix (cv, 0.5 + 0.5 * cv, 0.20 * paled * (1.0 - smoothstep (0.5, 1.0, halo)));
#else
    cv = mix (cv, 0.5 + 0.5 * cv, 0.35 * smoothstep (0.0, 0.5, halo) * (1.0 - smoothstep (0.5, 1.0, halo)));
#endif
    cv *= 1.0 - 0.38 * smoothstep (0.45, 1.0, halo);
    vec3 col = cv < 0.5 ? mix (shadow, mid, cv * 2.0) : mix (mid, light, cv * 2.0 - 1.0);

    // ---- the rift's light: lit vapour seen through the tear and carried out of it by the flow.
    // A noise-eroded gradient with dark strands across it, never a filled oval; some seeds and some
    // moments have none at all.
    vec2 mc = vec2 (p.x, p.y - (uMaw.x - open * 0.15));
    vec2 mr = vec2 (uMaw.y * 1.7, 0.03 + open * 0.85);
    vec2 ms2 = mc / mr;
    // (only where the ink stands round it: no tear and no light hang in the open cloud)
    float mOpen = smoothstep (0.015, 0.10, open) * smoothstep (0.15, 0.35, halo);
    vec3 accent = mix (vec3 (1.0, 0.74, 0.48), vec3 (1.0, 0.93, 0.78), 0.6 * uMood.z);
    // (the breath is vapour, not ink: its own picture, not folded. Light in a mirrored pair of
    // spots either side of the fold would be a pair of eyes.)
    vec4 bn = NZ (vec2 (p.x * 1.3 + 0.37, m.y * 0.8) - vw * 1.2 + vec2 (0.0, -uBreath.z) + uInkO.yx, max (1.0, PXL (1.3)));
    // (and in tall streaks that rise)
    vec4 wn = NZ (vec2 (p.x * 4.6, p.y * 2.0) + vec2 (0.0, -3.0 * uBreath.z) - uInkO.xy * 2.0, max (0.5, PXL (4.6)));
    float streak = smoothstep (0.25, 0.70, wn.a);
    float vap = smoothstep (0.30, 0.80, sv (bn) + 0.25 * bn.a - 0.35 * dot (ms2, ms2));
    float deep = 1.0 - smoothstep (-0.6, 0.9, ms2.y);                     // brighter low in the tear
#ifdef INK_BUFFER
    // (the light is the breath itself, where the flow has carried it: ragged, drifting, sometimes
    // none. It stands in wisps narrower than the tear that rise through it, brightest low down and
    // gone before the top: never the tear's own shape filled with light.)
    float wisp = smoothstep (0.39, 0.64, 0.65 * sv (wn) + 0.35 * sv (bn) + 0.10 * min (breathLit, 1.0) - 0.14 * smoothstep (-0.2, 0.9, ms2.y));
    float glow = 0.95 * min (breathLit, 1.0) * wisp * (0.25 + 0.75 * deep) * (0.55 + 0.45 * streak);
#else
    float glow = uBreath.x * mOpen * vap * (0.35 + 0.65 * deep) * exp (-0.8 * dot (ms2, ms2)) * (0.55 + 0.45 * streak);
    // the breath outside it: a plume rising and drifting with the flow, ragged
    vec2 pc = vec2 (p.x, p.y - (uMaw.x + 0.35 * open + 0.06));
    float plume = exp (-(pc.x * pc.x) / 0.035 - (pc.y * pc.y) / 0.06) * smoothstep (0.35, 0.75, sv (bn) + 0.3 * bn.a);
    glow += uBreath.x * uBreath.y * plume * 0.55 * F * smoothstep (0.015, 0.16, open) * (0.55 + 0.45 * streak);
#endif
    // (inside the tear no smoke hangs: what shows is the cloud deep behind, shaded, lit or not)
    float tear = mOpen * (1.0 - smoothstep (0.6, 1.0, length (ms2 * vec2 (1.2, 1.2))));
    // (what shows through the tear is the cloud deep behind the body, in its shade: unlit, a dark
    // tear in the dark; never a pale mark on the fold)
#ifdef INK_BUFFER
    // (the tear's own torn shape, nearly as dark as the ink round it)
    // (dark from where the ink begins to thin: no pale line round the tear)
    col *= 1.0 - min (1.0, 3.0 * tearSoft) * (0.90 + 0.08 * (sv (rn) - 0.5));
#else
    // (only in the tear itself, whatever its shape: a thin wash of ink over it is not darkened into an oval)
    col *= 1.0 - 0.78 * tear * (1.0 - ink);
#endif
#ifdef INK_BUFFER
    // (lit vapour, warm and dim at its edges: never a white lamp)
    col = mix (col, light * mix (vec3 (1.0), accent, 0.55 + 0.35 * (1.0 - glow)) * (0.55 + 0.45 * glow), clamp (glow, 0.0, 1.0) * (1.0 - ink));
    float clear = clamp (max (glow * 1.5, tearSoft), 0.0, 1.0);
#else
    col = mix (col, light * mix (vec3 (1.0), accent, 0.55), clamp (glow, 0.0, 1.0) * (1.0 - ink));
    float clear = clamp (max (glow * 1.5, tear), 0.0, 1.0);
#endif
    smoke *= 1.0 - 0.9 * clear;
    veil *= 1.0 - 0.9 * clear;

    // ---- pigment: subtractive, so overlaps deepen; thin ink splits towards a second hue. The
    // inside is marbled by where the ink came from.
    vec3 sigma = moodTurnBy (vec3 (1.05, 0.92, 0.74), 0.25);
    vec3 sigThin = sigma.zxy * 0.6 + 0.35;
#ifdef INK_BUFFER
    float marble = clamp (held.b * 1.2, -1.0, 1.0);
#else
    float marble = sv (NZ (gCarried * 0.45 + uInkO.yx, 2.0 + gStretch)) * 2.0 - 1.0;
#endif
    float veins = NZ (gCarried * 1.3 - uInkO.xy, max (1.0, PXL (1.3)) + gStretch).a;
    // some blots are one dense mass; others are marbled: part of the body is a thin wash the cloud
    // shows through, with the dense ink left as ribbons and veins in it
    float wash = smoothstep (0.30, 0.80, uInkO.w) * smoothstep (-0.25, 0.35, marble + 0.9 * (0.45 - veins));
    // (the heart of the body stays dense, to a ragged bound: the rift is a tear in the dark, never
    // a mark on a pale ground)
    wash *= smoothstep (0.08, 0.38, length (vec2 (p.x, 0.5 * (p.y - uFlowC.w))) + 0.28 * marble + 0.20 * (veins - 0.5));
#ifdef INK_BUFFER
    wash *= heldShare;   // (ink the body has let go is ink, whatever the body's own marbling was)
#endif
    // ink that is still gathering, or leaving, is thin where there is little of it
    float thinEdge = mix (2.0, 0.9, smoothstep (0.0, 0.45, un));
    float A = (ink * (thinEdge + (4.6 - thinEdge) * thick) * (1.0 + 0.55 * marble * thick) + rim * 0.8 * (1.0 - 0.6 * smoothstep (0.0, 0.45, un))) * uInkF.w;
    float Aw = ink * (0.55 + 0.9 * thick * (1.0 - veins)) + rim * 0.5;
    col *= exp (-sigma * mix (A, 0.0, wash)) * exp (-sigThin * (veil * 0.75 + 1.5 * Aw * wash)) * exp (-mix (sigThin, sigma, smokeDense) * smoke * SMOKE_DEPTH);

    // ---- wet sheen on the thick ink (the light is above and to the left): one broad soft band
    vec2 n2 = grad * 5.0 + gv (det) * 0.9 * 0.004;
    float nl2 = length (n2);
    float spec = pow (clamp (0.5 - 0.5 * dot (n2 / (nl2 + 1.0e-4), L), 0.0, 1.0), 4.0) * smoothstep (0.12, 0.6, nl2);
    // (no gleam on the rift's rim: a ring of light round a dark opening is an eye)
    col += light * spec * ink * (0.10 + 0.08 * roar) * smoothstep (0.9, 1.8, length (ms2)) / (1.0 + 2.0 * gStretch);
    col += light * thick * 0.035 * smoothstep (0.45, 0.75, detV);

    // ---- the cloud in front: optical depths with banks and gaps, never a flat layer. The body
    // parts it where it is deepest and the roar thins it; its banks drift across the rest of the
    // creature and may hide it, and the ink behind shades them.
    // (how far the body parts the cloud in front of itself is the blot's own: some stand clear,
    // some are half hidden)
    float clearing = 0.95 - 0.40 * clamp ((uCloudK.y - 0.55) / 0.8, 0.0, 1.0);
    float thin = (1.0 - uCloudK.w) * (1.15 - 0.3 * F);
#ifdef INK_BUFFER
    // (deepest: the ink over a wide round. Its heart stands clear; limbs and edges are left to the banks.)
    vec4 round = inkRead (m, 5.0);
    float deepInk = smoothstep (0.35, 0.80, round.r + 1.1 * max (round.g - inkFloor, 0.0));
    // (ink let go still parts the cloud while it hangs dense: the banks close over it as it thins,
    // not the moment the hold gives)
    deepInk = max (deepInk, 0.8 * smoothstep (0.15, 0.7, loose));
    // (the tear is in the body's shade and behind the same parted cloud as the ink round it)
    float behind = max (clamp (inkRead (m, 2.5).r, 0.0, 1.0), hole);
    float inkHere = max (ink * smoothstep (0.0, 0.5, thick), hole);
    float holeN = hole;
    if (uInkTier.x > 2.5)
    {
        // the middle layer: wide soft banks close in front of the ink
        vec3 midC = cloudLayer (p, capw, share, 0.16, 0.44, uCloudO.zw * 0.55 + uCloudO.xy * 0.6 + 0.23, 3.2, 2.2);
        float mx = midC.x * 2.0 - 1.08;
        float md = 0.5 * (mx + sqrt (mx * mx + 0.02));
        float partM = clamp (max (clearing * deepInk, max (0.45 * inkHere, 0.97 * hole)), 0.0, 1.0);
        // (the thin haze between the banks is behind no ink: in a gap the ink shows as dark as it is)
        float aM = 1.0 - exp (-(uCloudK.y * 3.0 * md * thin * (1.0 - partM) + 0.5 * uCloudK.z * (1.0 - 0.9 * inkHere)));
        float mlM = clamp (0.55 + dot (midC.yz, L) * 0.34, 0.0, 1.0);
        col = mix (col, mix (mid, light, mlM) * (1.0 - 0.32 * behind), aM);
    }
#else
    float deepInk = smoothstep (0.30, 0.75, halo);
    float behind = ink * smoothstep (0.0, 0.5, thick);
    float inkHere = behind;
    float holeN = tear;
#endif
    vec3 nearC = cloudLayer (p, capw * 1.45, share, 0.20, 0.55, uCloudO.zw, 3.0, 2.0);
    vec4 f3 = NZ (p * 1.5 - capw * 0.45 + nearC.x * 0.06 + uCloudO.zw * 1.7, max (1.0, PXL (1.5)));
    float fx = nearC.x * 2.0 + (sv (f3) - 0.5) * 0.22 - 1.02;
    float fd = 0.5 * (fx + sqrt (fx * fx + 0.02));
    float part = clamp (max (clearing * max (deepInk, 0.35 * inkHere), 0.97 * holeN), 0.0, 1.0);
    float tauN = uCloudK.y * 6.0 * fd * thin * (1.0 - part) + uCloudK.z * (1.0 - 0.9 * inkHere);
    float aN = 1.0 - exp (-tauN);
    float ml = clamp (0.62 + dot (nearC.yz + gv (f3) * 1.5 * 0.12, L) * 0.30, 0.0, 1.0);
    vec3 mistCol = mix (mid, light, ml) * (1.0 - 0.25 * smoothstep (0.2, 0.9, halo)) * (1.0 - 0.22 * behind);
    col = mix (col, mistCol, aN);
    // the breath scattered in the near cloud
    vec2 sc = mc / vec2 (0.24, 0.17);
    // (in the cloud, where there is cloud to scatter it: never a stain on the bare ink)
    // (and ragged with it: a round stain of light about the rift would be a lamp's)
    col += accent * exp (-dot (sc, sc)) * smoothstep (0.25, 0.75, sv (bn) + 0.3 * bn.a) * uBreath.x * mOpen * (0.05 + 0.14 * uBreath.y) * (0.15 + 0.85 * aN);
    // and drifting out in front of the body: ragged lit vapour the flow carries up and away
    vec2 bc = vec2 (p.x, p.y - uMaw.x - 0.04 - 0.10 * uBreath.y) + (bn.gb - vec2 (0.5)) * 0.5;
    float reachB = dot (bc / vec2 (0.20 + 0.10 * uBreath.y, 0.14 + 0.08 * uBreath.y), bc / vec2 (0.20 + 0.10 * uBreath.y, 0.14 + 0.08 * uBreath.y));
#ifdef INK_BUFFER
    // (ragged patches of it, however much breath there is: never a round stain over the ink)
    float vapour = smoothstep (0.44, 0.74, 0.55 * sv (wn) + 0.45 * sv (bn) + 0.08 * min (breathWide, 1.0)) * clamp (2.5 * breathWide, 0.0, 1.0);
#else
    float vapour = smoothstep (0.42, 0.90, sv (bn) + 0.35 * bn.a + 0.15 - reachB) * uBreath.x * smoothstep (0.015, 0.16, open) * F;
#endif
    // (streaked: brighter and dimmer along the wisps, never a flat patch)
    vapour *= 0.55 + 0.45 * streak;
    // (light added to what is there, the ink included: lit vapour in front of the dark, never a tan stain on it)
    col = vec3 (1.0) - (vec3 (1.0) - col) * (vec3 (1.0) - 0.60 * vapour * light * mix (vec3 (1.0), accent, 0.7));

    // vignette, grain, dither
    vec2 uvq = frag / uRes;
    col *= 0.90 + 0.10 * smoothstep (0.0, 0.25, uvq.x * (1.0 - uvq.x) * 4.0) * smoothstep (0.0, 0.3, uvq.y * (1.0 - uvq.y) * 4.0);
    col += (hash12 (frag + 3.1) - 0.5) * 0.010;
    col += outDither (frag);
    col = clamp (col, 0.0, 1.0);

    // rounded corners, filled with what is around the view
    vec2 cq2 = abs (frag - 0.5 * uRes) - (0.5 * uRes - vec2 (uRadius));
    float dc = length (max (cq2, 0.0)) - uRadius;
    col = mix (col, uColOuter, smoothstep (-0.75, 0.75, dc));

    gl_FragColor = vec4 (col, 1.0);
}
)GLSL";

}

const char* vertex() { return c_vertex; }

std::string fragment (Program program, bool textureLod)
{
    std::string s;
    if (program == Program::inkSinglePass || program == Program::inkSim || program == Program::inkComposite)
    {
        // the noise is read at an explicit level of detail where the context can (every core
        // context; older ones through the extension the caller enables), so a jump in the lookup
        // position never picks a wrong mip
        s += textureLod ? "#define NZ(uv, lod) texture2DLod (uNoise, uv, lod)\n" : "#define NZ(uv, lod) texture2D (uNoise, uv)\n";
        if (program != Program::inkSinglePass)
            s += "#define INK_BUFFER 1\n#define IT(uv, lod) texture2DLod (uInkTex, uv, lod)\n";
        s += c_inkCommon;
        if (program != Program::inkSinglePass)
            s += c_inkBuffer;
        // (the buffer's step and the draws share everything above: the tiers cannot drift apart)
        s += program == Program::inkSim ? c_inkSim : c_inkSingle;
        return s;
    }
    const auto creature = [&s] (const char* smk)
    {
        s += "#define SMK ";
        s += smk;
        s += "\n";
        s += c_creatureUniforms;
        s += c_noise;
        s += c_helpers;
        s += c_atmo;
    };
    const auto sim = [&s] (bool cloudTarget)
    {
        s += c_simCommon;
        s += c_noise;
        if (cloudTarget)
            s += c_cloudTarget;
    };

    switch (program)
    {
        case Program::creatureField:
            s += c_cfPre;
            creature ("1.5");
            s += c_cfMainPre;
            s += c_head;
            s += c_body;
            s += c_face;
            s += c_cfPost;
            break;
        case Program::emitter:    sim (false); s += c_emitter; break;
        case Program::advect:     sim (false); s += c_advect; break;
        case Program::curl:       sim (false); s += c_curl; break;
        case Program::forces:     sim (false); s += c_forces; break;
        case Program::divergence: sim (false); s += c_divergence; break;
        case Program::jacobi:     sim (false); s += c_jacobi; break;
        case Program::gradient:   sim (false); s += c_gradient; break;
        case Program::dye:        sim (true);  s += c_dye; break;
        case Program::light:      sim (false); s += c_light; break;
        case Program::bright:     s += c_bright; break;
        case Program::down:       s += c_down; break;
        case Program::up:         s += c_up; break;
        case Program::composite:
            s += c_compPre;
            creature ("1.5");
            s += c_mist;
            s += c_compMainPre;
            s += c_head;
            s += c_face;
            s += c_compPost;
            break;
        case Program::copy:       s += c_copy; break;
        case Program::probe:      s += c_probe; break;
        case Program::singlePass:
            s += c_q0Pre;
            creature ("1.0");
            s += c_mist;
            s += c_q0Main;
            s += c_head;
            s += c_body;
            s += c_face;
            s += c_q0Occ;
            s += c_q0H;
            s += c_q0Rest;
            break;
        case Program::inkSinglePass:
        case Program::inkSim:
        case Program::inkComposite: break;   // (above)
        case Program::numPrograms: break;
    }
    return s;
}

const char* name (Program program)
{
    static const char* const names[] = { "creature field", "emitter", "advect", "curl", "forces", "divergence", "jacobi",
                                         "gradient", "dye", "light", "bright", "down", "up", "composite", "copy", "probe",
                                         "single pass", "ink single pass", "ink step", "ink composite" };
    const auto i = static_cast<int> (program);
    return i >= 0 && i < static_cast<int> (Program::numPrograms) ? names[i] : "?";
}
}
