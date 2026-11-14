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
uniform vec4 uMaw;       // the rift: height on the fold, half width, opening, how uneven its edge is
uniform vec4 uBay[2];    // bays: centre xy, radius, direction
uniform vec4 uFlowK;     // eddy gain, small-eddy share, bound pair strength, 1 / its radius squared
uniform vec4 uFlowO;     // where the large eddies are read xy, the small ones xy
uniform vec4 uFlowC;     // the bound pair's centre xy, spare, the body's centre height
uniform vec4 uCarry;     // carry time formed, extra carry unformed, how unformed (0 formed .. 1 cloud), the smoke's further carry
uniform vec4 uInkF;      // formation, roar (capacity), feather, density
uniform vec4 uInkO;      // the blot's noise offset xy, a slow scroll, ribbon
uniform vec4 uCloudK;    // mirror share, bank opacity, gap opacity, roar thinning
uniform vec4 uCloudO;    // far layer drift xy, near layer drift xy
uniform vec4 uBreath;    // the rift's light now, the breath's jet, its rise (a scroll), level
uniform vec4 uCloudT;    // the far cloud's two carries (seconds), the second's share, the flow at rest over the flow now
// the modules' traces: each a change to the shape, the flow or the material, none a tint
uniform float uBands[32];
uniform vec4 uModA;      // DETAIL: broad scallops, hairline fringe; VOCODER: clotting, strata
uniform vec4 uModB;      // RING: depth, phase, ripples per view height; GROWL: depth
uniform vec4 uModC;      // GROWL: phase; ENSEMBLE: the echoes' spacing; SHIMMER: beads, their phase
uniform vec4 uModD;      // OVERTONE: the core's thinning; DE-ESS: spray; SUB: the pool under the body; the trace budget's scale
uniform vec4 uModE;      // CLEAN: tidiness; the wisps' sway (a phase); spare
uniform vec4 uChamber;   // CHAMBER: the room's ringing, its walls, the bands' phase, the shadow's length
uniform vec4 uSpine[5];  // OVERTONE: each wisp's root xy, length, lean
// the maker's mark, hidden: now and then the ink gathers into it
uniform float uEgg;
uniform vec4 uLogoBox;   // centre xy, box size
uniform sampler2D uLogoSdf;   // R the dark group's distance, G the red group's

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

// Distance to the mark at a point of the view (not folded); gLogoRed: 1 in its red group.
float gLogoRed;
float logoDist (vec2 p)
{
    vec2 luv = (p - uLogoBox.xy) / uLogoBox.z + 0.5;
    vec4 L = texture2D (uLogoSdf, clamp (luv, 0.0, 1.0));
    float boxD = length (max (abs (luv - 0.5) - 0.5, 0.0)) * uLogoBox.z;
    // (capped where the baked field saturates: a saturated field meeting the box's distance is a kink
    // along the box's edge, a faint rectangle drawn in the smoke)
    float dG = min ((L.r - 0.5) * 0.25 * uLogoBox.z, 0.10 * uLogoBox.z) + boxD;
    float dC = min ((L.g - 0.5) * 0.25 * uLogoBox.z, 0.10 * uLogoBox.z) + boxD;
    gLogoRed = dC < dG ? 1.0 : 0.0;
    return min (dG, dC);
}

// The flow at a point of the half plane (x >= 0), view heights per second. Its stream function is
// odd across the fold, so the fold is a streamline: the large eddies are the noise minus its mirror
// image, the small ones fade out at the fold, and one mirrored pair of vortices is bound to the body.
vec2 flowAt (vec2 m, float gain)
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
    vec2 v = vec2 (g.y, -g.x) * gain;
    vec2 d1 = m - uFlowC.xy, d2 = vec2 (m.x + uFlowC.x, m.y - uFlowC.y);
    float e1 = exp (-dot (d1, d1) * uFlowK.w), e2 = exp (-dot (d2, d2) * uFlowK.w);
    v += uFlowK.z * vec2 (-d1.y * e1 + d2.y * e2, d1.x * e1 - d2.x * e2);
    // the ceiling: nothing carries ink faster than 0.6 view heights a second
    return v * inversesqrt (1.0 + dot (v, v) / 0.36);
}
vec2 flow (vec2 m) { return flowAt (m, uFlowK.x); }
// (the same eddies at their rest pace: what the carry is traced through)
vec2 flowRest (vec2 m) { return flow (m) * uCloudT.w; }

// Where the ink at m was tau seconds ago (four backward steps through the flow).
vec2 backtrace (vec2 m, float tau)
{
    vec2 q = m;
    float h = tau * 0.25;
    for (int i = 0; i < 4; ++i)
    {
        q -= flowRest (q) * h;
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

// The rift at a point of the half plane: negative in the open tear, positive in the ink round it.
float gRift;
float gSide = 1.0;   // which side of the fold the point being drawn is on (the tear's torn edge is not folded)
float gWander = 0.0; // 1 where the whole view is drawn at once (the tear may leave the fold)
float gSoft = 0.0;   // the soft fold's width, where the point came through it
float riftField (vec2 q)
{
    float open = uMaw.z;
    // the rift: a tear on the fold, taller than wide when open and shut when the jaw is; its outline
    // is eroded by noise that travels with the ink
    // (torn wide only in the roar: in speech it is the narrower tear, and the body round it stays whole)
    vec2 mr = vec2 (uMaw.y * (0.8 + (1.1 + 0.5 * uInkF.y) * smoothstep (0.0, 0.12, open)), 0.005 + open * (0.80 + 0.45 * uInkF.y));
    // (its middle wanders either side of the fold as it rises: a tear, not a slot cut on the fold.
    // Not in the buffer's shape, which is folded: there it would be a pair.)
    float xs = gSide * max (q.x - gSoft, 0.0);
    vec2 mc = vec2 (xs - gWander * mr.x * 0.7 * (NZ (vec2 (0.23, q.y * 1.7) + uInkO.xy, 1.0).r - 0.5) * smoothstep (0.0, 0.06, open), q.y - (uMaw.x - open * 0.15));
    // (a crack, not an oval: pointed at both ends, its width uneven along its length)
    float ty = clamp (mc.y / mr.y, -1.0, 1.0);
    float uneven = clamp (sv (NZ (vec2 (0.61, q.y * 1.3) + uInkO.xy, 1.0)), 0.0, 1.0);
    // (its edge is torn by noise that travels with the ink, but only ever narrows or widens the one
    // opening on the fold: an opening torn off to one side would be mirrored into a pair)
    float torn = clamp (sv (NZ (vec2 (xs, q.y) * 4.5 + uInkO.yx, max (0.5, PXL (4.5)))), 0.0, 1.0);
    float rw = mr.x * pow (1.0 - ty * ty, 1.1) * (0.35 + 1.5 * uneven) * (0.55 + 0.9 * torn);
    float rift = max (abs (mc.x) - rw, abs (mc.y) - mr.y) + (1.0 - smoothstep (0.0, 0.02, open)) * 0.25;
    // (nothing is left across it: strands of ink across a lit tear, however unevenly they are
    // spaced, are a row of teeth. The tear is one ragged opening.)
    gRift = rift;
    return rift;
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
#ifndef INK_SIM
    // (the buffer's shape has no tear: the draw cuts it, from the jaw as it is now and unfolded)
    d = smax (d, -riftField (q), 0.03);
#endif
    // the bays: tapered cuts that widen towards the outside, so the cloud they let in is never enclosed
    for (int i = 0; i < 2; ++i)
    {
        vec4 b = uBay[i];
        float e = sdSeg (p, b.xy, b.xy + 1.5 * vec2 (cos (b.w), sin (b.w)), b.z, b.z * 3.0);
        d = smax (d, -e, 0.03);
    }
    return d;
}

// OVERTONE: wisps of ink drawn up out of the body's top, one for each harmonic, as long as it is
// loud. Each leaves the body its own way, is bent by the flow that carries it, tapers to nothing
// and is torn along its length: ink the eddies have pulled out, never a drawn stroke, and never
// several side by side (a row of strokes is a comb, a claw or a set of teeth; a short one is a dot).
// Too thin for the buffer: the passes that draw draw them from this. Returns how much ink, 0..1.
float spineInk (vec2 m, vec2 v, float tear)
{
    float ink = 0.0;
    for (int i = 0; i < 5; ++i)
    {
        vec4 s = uSpine[i];
        // (none until it is long enough to be a wisp)
        float grown = smoothstep (0.045, 0.10, s.z);
        if (grown <= 0.0)
            continue;
        vec2 dir = normalize (vec2 (s.w, 1.0));
        vec2 side = vec2 (dir.y, -dir.x);
        vec2 l = m - s.xy;
        float along = clamp (dot (l, dir) / max (s.z, 1.0e-3), 0.0, 1.0);
        // (carried by the flow and swaying, more towards its end: it bends as the ink round it moves)
        float bend = along * along * (0.45 * dot (v, side) / (0.05 + length (v)) * s.z + 0.16 * s.z * sin (uModE.y + float (i) * 1.7)
                                      + 0.5 * s.z * (NZ (vec2 (along * 0.9 + float (i) * 0.31, 0.17) + uInkO.xy, 1.0).r - 0.5));
        float d = length (l - dir * along * s.z - side * bend);
        // (wide and soft at the root, where it leaves the ink, down to nothing: no end to see)
        float w = mix (0.022, 0.0, along * along) * (0.6 + 0.8 * tear);
        float e = (1.0 - smoothstep (0.25 * w, w + 1.0e-4, d)) * (1.0 - smoothstep (0.55, 1.0, along));
        // (torn along its length: whole in places, gone in others)
        e *= smoothstep (0.20, 0.52, NZ (vec2 (along * 2.3 + float (i) * 0.47, d * 9.0) + uInkO.yx, 1.0).r + 0.40 * (1.0 - along));
        ink = max (ink, e * grown);
    }
    return ink;
}

// The modules' traces on the shape, in the frame the ink was carried from (they travel with it):
// each bends the edge its own way, and together they share one soft limit, so ten of them at once
// bend the blot and do not turn it to mush. Anything fine fades out at the fold, where a pattern
// would meet its own mirror image and stand as a column of beads.
// VOCODER's strata at a point of the (folded) view: the edge's fray drawn out sideways, in six
// layers that sag and wander with the blot's own scroll, each torn as deep as its bands are quiet.
float strataFray (vec2 q, float fineV)
{
    vec4 sn = NZ (vec2 (q.x * 0.6, q.y * 0.5) + uInkO.xy * 1.9 + vec2 (0.0, 0.5 * uInkO.z), 1.5);
    float y = q.y + 0.09 * (sv (sn) - 0.5) - uFlowC.w;
    float by = clamp ((y + 0.42) / 0.84, 0.0, 0.999) * 6.0;
    int bi = int (floor (by) * 5.0);
    float lv = clamp (1.6 * max (uBands[bi], uBands[bi + 2]), 0.0, 1.0);
    // (stretched along the layers and ragged along them)
    // (broad layers: a dozen to a view height, in thin ink, are an engraver's hatching)
    float n = sv (NZ (vec2 (q.x * 1.2 + 0.3 * fineV, y * 7.0) + uInkO.yx * 2.0, max (0.5, PXL (7.0))));
    return uModA.w * (0.30 + 0.40 * (1.0 - lv)) * (n - 0.55);
}

float moduleTerms (vec2 q, float d0, float stretch)
{
    float t = 0.0;
    float offFold = smoothstep (0.02, 0.14, q.x);
    // DETAIL: the calibre of the edge's fingers, broad scallops (low band) and a hairline fringe (high)
    t += uModA.x * 0.075 * (sv (NZ (q * 1.25 + uInkO.yx * 2.0, 1.5 + stretch)) - 0.5);
    // (both even about the edge: a calibre, never the blot grown or shrunk)
    t += uModA.y * 0.034 * (sv (NZ (q * 7.0 - uInkO.xy * 3.0, max (0.0, PXL (7.0)) + stretch)) - 0.5) * 1.4 * offFold;
    // VOCODER: gravel clots the ink (its strata are drawn by the passes that draw)
    t += uModA.z * 0.050 * (NZ (q * 9.0 + uInkO.xy * 5.0, max (0.0, PXL (9.0)) + stretch).r - 0.5) * 2.0 * offFold;
    // RING: ripples spreading from the rift, spaced by the carrier
    float rr = length (vec2 (q.x, q.y - uMaw.x));
    // (not on the fold, where a ripple crossing it would open a round hole on the middle line)
    t += uModB.x * 0.050 * sin (rr * uModB.z - uModB.y + 1.5 * (sv (NZ (q * 0.9 + uInkO.yx, 1.5 + stretch)) - 0.5)) * smoothstep (0.08, 0.2, rr) * (1.0 - smoothstep (0.45, 0.95, rr))
       * smoothstep (0.04, 0.16, q.x);
    // GROWL: a ruffle running along the outline at the growl's own rate
    float ang = atan (q.y - uFlowC.w, q.x + 0.02);
    // (unevenly: its pitch and its depth wander along the outline. Evenly spaced it is a row of ticks.)
    float ruf = sv (NZ (q * 1.6 + uInkO.xy * 2.0 + 0.53, 1.5 + stretch));
    t += uModB.w * 0.036 * sin (ang * 21.0 + 5.0 * rr + uModC.x + 9.0 * (ruf - 0.5)) * (0.35 + 1.3 * smoothstep (0.30, 0.70, ruf))
       * (1.0 - smoothstep (0.0, 0.14, abs (d0))) * offFold;
    // OVERTONE, replacing the voice: the core thins as the wisps grow
    t += uModD.x * 0.055 * (1.0 - smoothstep (0.05, 0.38, length (vec2 (q.x, q.y - uFlowC.w))));
    t *= uModD.w;
    return t / (1.0 + abs (t) / 0.08);
}

// What inkTarget leaves behind for the passes that draw.
vec2 gCarried;      // where this point's ink came from
float gStretch;     // how many levels coarser the carried frame must be read (the carry stretches it)
float gHaloD;       // the unfeathered distance, eroded while unformed
float gFringe;      // the same field carried further, wider and more eroded (smoke)
float gEdge;        // how far into the band at the view's top or bottom, where no ink stands

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
    float ry = clamp (m.y, uMaw.x - 1.35 * uMaw.z, uMaw.x + 1.05 * uMaw.z);
    float nearRift = 1.0 - smoothstep (0.0, 0.12, length (vec2 (m.x, m.y - ry)));
    tau *= 1.0 - 0.7 * nearRift * (1.0 - un) * (1.0 - un);
    // (and along the fold, where the eddies of both sides run together as one stream: formed, the
    // blot is not drawn out into a column on it)
    tau *= mix (1.0, mix (0.4, 1.0, smoothstep (0.0, 0.22, m.x)), (1.0 - un) * (1.0 - un));
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
    // (the fine fray fades out at the fold: a pattern meeting its own mirror image there is a row of chevrons)
    float feather = ((sv (n1) - 0.5) * (0.09 + 0.10 * un) + (sv (n2) - 0.5) * 0.03 * smoothstep (0.0, 0.10, m.x)) * uInkF.z * (1.0 - 0.6 * nearRift);
    // erosion while unformed: the outside goes first, in patches the noise picks
    // (the floor takes what is thin first, everywhere: the limbs go before the core)
    float erosion = un * un * (0.13 + 2.1 * patch * patch) + smoothstep (0.7, 1.0, un) * 0.5;
    // (nothing is cut off by the view's edge: towards the top and the bottom the ink gives way to cloud)
    float edge = smoothstep (0.37, 0.48, abs (m.y)) * 0.60;
    gEdge = edge;
    float traces = moduleTerms (q, d0, gStretch);
    float d = d0 + feather + traces;
    if (uEgg > 0.001)
    {
        // the mark: the shape the ink may fill crosses over to it. The buffer is folded and the mark
        // is not, so there it is the mark and its mirror image together (the draw keeps only the true
        // one); a pass that draws the whole view takes the mark itself.
#ifdef INK_BUFFER
        float dl = min (logoDist (m), logoDist (vec2 (-m.x, m.y)));
#else
        float dl = logoDist (vec2 ((m.x - gSoft) * gSide, m.y));
#endif
        float eggK = smoothstep (0.05, 0.75, uEgg);
        d = mix (d, dl + (sv (n2) - 0.5) * 0.012, eggK);
        d0 = mix (d0, dl, eggK);
        edge *= 1.0 - eggK;
    }
    d += erosion + edge;
    gHaloD = d0 + erosion + edge;   // (d0: the mark's own distance while the ink is in it)
    // the smoke fringe: the same anatomy carried further down the flow (two more steps)
    vec2 q2 = q - flowRest (q) * uCarry.w;
    q2.x = abs (q2.x);
    q2 -= flowRest (q2) * uCarry.w;
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
uniform vec4 uInkRule;   // the body's floor (0..1, long formed), how fast the hold is letting go (0..1), how heavy loose pigment is drawn (1: a wisp; more: the smoke of ink let go), the gathering's share of the pull
uniform vec4 uHold;      // hold, release (per second), drift (+ inwards, - outwards; view heights a second), its reach
uniform vec4 uPig;       // the cloud's own pigment, the budget's gain on it, the release's curl kick, sink
uniform vec4 uInkTier;   // cloud layers (2 or 3), 1: the far cloud is carried in two phases, 1: threads, how lately the body was let go (0..1)
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
        // (its slope from four taps a texel and a half apart, so that it turns from one pixel to the
        // next: a slope from screen derivatives is one for each block of four pixels on some
        // drivers, and then the spread below never sees the way inwards bend, and loses the
        // pigment that crowds as the rings round the body grow shorter)
        float h = 1.5 / uInkRes.y;
        float dxp = anatomySdf (vec2 (sqrt ((m.x + h) * (m.x + h) + 0.0004), m.y)), dxm = anatomySdf (vec2 (sqrt ((m.x - h) * (m.x - h) + 0.0004), m.y));
        vec2 gA = vec2 (dxp - dxm, anatomySdf (ms + vec2 (0.0, h)) - anatomySdf (ms - vec2 (0.0, h)));
        vec2 dir = gA / (length (gA) + 1.0e-6);
        // (the pull eases off over a wide band as it reaches the body, so the pigment gathers as a
        // soft hull of smoke round where the body will be, never as its outline or its skeleton)
        // (and it stops at the rim: the body takes in what arrives there. Carried on to the body's
        // middle, the pigment met itself head on at the middle line and was lost there: the step
        // reads it from beyond the line every time and it can never pile up)
        float pull = uHold.z > 0.0 ? (1.0 - smoothstep (0.5 * uHold.w, uHold.w, dA)) * smoothstep (-0.05, 0.25, dA)
                                   : smoothstep (-0.12, 0.0, dA) * (1.0 - smoothstep (0.0, 0.5, dA));
        // (no pull across the fold: pigment drawn onto it from both sides would pile into a line)
        // (and none towards the view's top or bottom, where no ink may stand: nothing piles up there)
        pull *= 1.0 - smoothstep (0.40, 0.49, abs (m.y));
        // (and while it lets go, none along the fold either: pushed only up and down there, what the
        // body leaves on the fold is driven into a column and piles up at its end)
        float onFold = smoothstep (0.0, 0.12, m.x);
        vec2 drift = -dir * uHold.z * pull * vec2 (onFold, uHold.z < 0.0 ? 0.25 + 0.75 * onFold : 1.0);
        // the kick: small eddies that take only the free pigment (none of it crosses the fold)
        vec4 kn = NZ (m * 1.9 + uInkO.xy * 3.0 + vec2 (0.0, 2.0 * uInkO.z), max (0.5, PXL (1.9)));
        float e = clamp (m.x / 0.2, 0.0, 1.0);
        float ks = e * e * (3.0 - 2.0 * e), dks = 6.0 * e * (1.0 - e) / 0.2;
        vec2 kg = gv (kn) * 1.9;
        vec2 kick = uPig.z * 0.02 * vec2 (ks * kg.y, -(ks * kg.x + dks * (sv (kn) - 0.5)));
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
        // DE-ESS: sibilants throw a fine spray of the pigment at the rim about the rift out into the
        // cloud, in specks (only off the fold: a spray mirrored onto itself there is a column of dots).
        // It moves pigment and makes none; the pull brings it back in when the de-esser lets go.
        vec2 rdS = vec2 (m.x, m.y - uMaw.x);
        vec2 spray = uModD.y * 0.35 * dir * smoothstep (-0.04, 0.0, dA) * (1.0 - smoothstep (0.10, 0.22, dA)) * exp (-dot (rdS, rdS) / 0.30)
                   * smoothstep (0.04, 0.10, m.x) * smoothstep (0.45, 0.70, NZ (m * 9.0 + vec2 (0.013, 0.007) * uInkStep.z, max (0.5, PXL (9.0))).a);
        // SUB: the loose pigment under the body drawn together into a dark pool that hangs there
        // (moved, not made: what gathers is taken from round it)
        vec2 pd = vec2 (m.x, m.y - (uFlowC.w - 0.27)) * vec2 (0.45, 1.0);
        float pr = length (pd);
        vec2 pool = -uModD.z * 0.10 * pd / (pr + 0.02) * smoothstep (0.02, 0.06, pr) * (1.0 - smoothstep (0.18, 0.32, pr));
        vec2 vG = v + drift + kick + breath + push + spray + pool - vec2 (0.0, uPig.w);
        // (drawn in, pigment gathers where the pull converges and the cloud pales where it spreads;
        // only the pull does this: the eddies carry pigment without ever piling it up)
        // (all of the pull, the slight one that goes on awake as well: carried without this, the
        // pull loses pigment where it crowds and makes it where it spreads, so it would take pigment
        // from round the body and the budget would put it back far out in the cloud)
        // (let go, the push outwards only carries it: thinned here as well, the body let go would pale
        // in a moment and darken again as the smoke gathered)
        float conv = uHold.z > 0.0 ? clamp (-(dFdx (drift.x) * uInkRes.x / uInkRes.z + dFdy (drift.y) * uInkRes.y) * dt, -0.08, 0.08) : 0.0;
        // (the pool gathers what its pull brings, as the body's pull does)
        float convP = clamp (-(dFdx (pool.x) * uInkRes.x / uInkRes.z + dFdy (pool.y) * uInkRes.y) * dt, -0.08, 0.08);

        vec4 a = inkRead (m - (v + 0.5 * push) * dt, 0.0);
        vec2 mg = m - vG * dt;
        // (it spreads a little as it goes; more beside the fold, which no flow crosses and which
        // would otherwise keep a stripe of whatever it last held)
        // (let go, more yet: what the body leaves on the fold spreads, and is not drawn out into bars)
        // (from a level only a little coarser: a coarse level read over and over is a coarse texel,
        // and leaves square blocks of pigment on the fold)
        float g = mix (inkRead (mg, 0.0).g, inkRead (mg, 1.2).g, 0.04 + (0.20 + 0.65 * uInkTier.w) * (1.0 - smoothstep (0.0, 0.10, m.x)));
        // (what arrives is the cloud's own pigment at the pull's pace: it adds up. Only while the body
        // gathers may it pile up past that; the slight pull that goes on awake piles nothing)
        g += conv * (conv > 0.0 ? mix (min (g, 1.5 * amb), g, uInkRule.w) : g);
        g += convP * g;
        // beads of pure pigment thrown from the rim by a clipped peak
        for (int i = 0; i < 3; ++i)
        {
            vec2 bd = m - uSparkP[i].xy;
            g += uSparkP[i].w * dt * exp (-dot (bd, bd) / (uSparkP[i].z * uSparkP[i].z));
        }
        float c = a.r, q = a.b;
        float b = inkRead (m - (v + breath) * dt, 0.0).a;

        // cloud shut inside the ink: an eddy that winds a limb round on itself, or two masses that
        // close round a gap, leave a pale hole with ink on all sides or all but one, and mirrored, a
        // pair of those is a pair of eyes. Wet ink runs in and closes it; a bay, open wide to the
        // outside, stays open.
        // (only once it has formed and while it holds: the shreds that gather and the smoke let go
        // may hold whatever gaps the flow leaves)
        // (the ink on the side where there is least, and on the side with the next least: one gap,
        // the mouth of a curl, does not keep the hole open)
        // (looked for at three reaches: a small hole, the wider eye of a curl, and the round a limb
        // wound right about by an eddy shuts in, which in the side masses is one of a pair of sockets)
        float shut = 0.0;
        for (int j = 0; j < 3; ++j)
        {
            float least = 9.0, next = 9.0;
            for (int k = 0; k < 8; ++k)
            {
                float ak = (float (k) + 0.5 * float (j)) * 0.785398;
                float side = inkRead (m + vec2 (cos (ak), sin (ak)) * (j == 0 ? 0.05 : j == 1 ? 0.09 : 0.14), j == 2 ? 2.5 : 1.5).r;
                next = min (next, max (least, side));
                least = min (least, side);
            }
            shut = max (shut, next);
        }
        // (and not in the maker's mark, which has holes of its own)
        float closing = smoothstep (0.35, 0.70, shut) * smoothstep (0.6, 0.95, uInkF.x) * (1.0 - uInkRule.y) * (1.0 - smoothstep (0.0, 0.3, uEgg));
        cap = max (cap, closing * min (shut * 1.2, 1.2));
        c += max (closing * min (shut, 1.0) - c, 0.0) * min (1.0, 4.0 * dt);

        // pigment condenses into ink (a little pigment is a lot of ink) and ink let go spreads into
        // smoke; while the hold lets go, ink leaves even where the shape still stands
        float take = min (uHold.x * (1.0 - uInkRule.y) * dt * max (cap - c, 0.0) / 3.0, g);
        // (let go in streaks, not all at once: some of the ink holds on a little longer than the rest)
        // (fine streaks, and none holds on for long: what is left is many shreds, never a pair of blots)
        float streak = NZ (m * 5.5 + uInkO.yx * 2.0 - v * 0.8, max (0.5, PXL (5.5))).a;
        float give = min (uHold.y * dt * max (c - cap, 0.0) + uInkRule.y * (0.9 + 1.5 * smoothstep (0.25, 0.75, streak)) * dt * c, c);
        // (and what the flow carries towards the view's top or bottom is let go at once)
        give = min (give + gEdge * 12.0 * dt * c, c);
        // (the fold goes first: let go last, the ink on it is a column left standing between two
        // dark remnants)
        float foldK = (1.0 - smoothstep (0.0, 0.07, m.x)) * max (uInkRule.y, 0.6 * uInkTier.w * uCarry.z);
        give = min (give + foldK * 2.5 * dt * c, c);
        q = (q * c + tone * 3.0 * take) / (c + 3.0 * take + 1.0e-4);
        c += 3.0 * take - give;
        // (awake, what the rim sheds is the pigment it was made of, a wisp; let go, the body's ink
        // spreads into heavy smoke)
        g += give / 3.0 - take;
        // (long formed, the body never empties, whatever is pulling at it and however little
        // pigment the cloud has left to give)
        c += max (0.7 * smoothstep (0.5, 0.8, cap) - c, 0.0) * uInkRule.x * min (1.0, 3.0 * dt);
        // away from the body the cloud comes back to its own pigment: slowly where it paled, sooner
        // where smoke hangs
        float away = 1.0 - smoothstep (0.0, 0.15, cap);
        float want = amb * uPig.y;
        // (smoke clears in patches, never evenly: the cloud opens holes in it)
        g += (want - g) * away * min (1.0, dt * (g > want ? 0.14 + 0.42 * smoothstep (0.40, 0.72, sv (kn)) : 0.04 + 0.4 * max (-uInkStep.w, 0.0)));
        // (loose pigment on the fold, which no flow crosses, clears soonest once the body is let go:
        // left there it stands as a pillar, and its streaks meet their mirror image in chevrons)
        g -= max (g - want, 0.0) * (1.0 - smoothstep (0.0, 0.07, m.x)) * uInkTier.w * min (1.0, 1.6 * dt);
        // the budget: with more pigment in the view than the cloud had, what hangs loose clears
        // sooner, wherever it is (in the same patches)
        g -= max (g - want, 0.0) * min (1.0, dt * max (uInkStep.w, 0.0) * (0.5 + 1.5 * smoothstep (0.30, 0.70, sv (kn))));
        // the breath: lit vapour fed low in the open rift, carried out, gone in a second
        float src = uBreath.x * jawOpen * (0.4 + 2.4 * uMaw.z) * smoothstep (0.25, 0.7, inkRead (m, 3.5).r) * exp (-(rc.x * rc.x) / (0.0009 + uMaw.y * uMaw.y) - (rc.y + 0.3 * uMaw.z) * (rc.y + 0.3 * uMaw.z) / (0.0009 + 0.3 * uMaw.z * uMaw.z));
        b = b * exp (-dt / 1.1) + 3.2 * dt * src;
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
    // the fold is not ruled: close to it the picture is read a little aside, by the view's own
    // noise (not folded), so what meets its mirror image there meets it unevenly. Thin ink that met
    // itself exactly on a straight line stood as chevrons, a dashed column or a seam.
    vec2 pf = p;
    {
        // (the maker's mark is drawn true)
        float nearFold = (1.0 - smoothstep (0.0, 0.16, abs (p.x))) * (1.0 - smoothstep (0.0, 0.3, uEgg));
        vec4 wb = NZ (p * vec2 (0.9, 1.7) + uInkO.yx + 0.41, 1.5);
        pf += nearFold * vec2 (0.030 * (sv (wb) - 0.5), 0.022 * (wb.a - 0.5));
    }
    float sgn = pf.x < 0.0 ? -1.0 : 1.0;
    vec2 m = vec2 (abs (pf.x), pf.y);
    vec2 ms = vec2 (sqrt (pf.x * pf.x + 0.0004), pf.y);  // the soft fold: no crease down the middle
    gSide = sgn;
    gWander = 1.0;
#ifndef INK_BUFFER
    gSoft = 0.02;
#endif
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
    // (loose pigment over the cloud's own, as heavy as it is drawn: a wisp awake, ink still when let go)
    float smokeK = 1.1 * uInkRule.z;
    // (and let go, over less of the cloud's own: round the body the gathering took the cloud's
    // pigment, and what the body lets go there is all there is; with the cloud's whole share taken
    // off it, ink let go would pale in a moment and darken again as it spread over pigment)
    float inkBase = uPig.x * max (1.0, uPig.y) * (1.0 - 0.6 * uInkTier.w), inkSlack = 0.2 * uPig.x / uInkRule.z;
    float heldShare = held.r / (held.r + smokeK * max (held.g - inkBase - inkSlack, 0.0) + 1.0e-3);
    float body = held.r + smokeK * max (held.g - inkBase - inkSlack, 0.0);
    // (the buffer's ink lags the shape a little, which widens its edge: drawn steeper about the edge)
    body = max (0.5 + (body - 0.5) * 1.7, 0.0);
    // (a small pale spot with dense ink all round it is drawn shut: the buffer closes such a hole
    // over a second or so, and until it has, or where a curl keeps one open, a mirrored pair of
    // them in the dark is a pair of eyes. Told from a bay by the ink over a wide round, which round
    // a small hole is nearly whole and at a bay's mouth is half cloud. Not on the fold, where the
    // one opening there may be is the rift; and not in the maker's mark, which has holes of its own.)
    vec4 round = inkRead (m, 5.0);
    body = max (body, 0.95 * smoothstep (0.74, 0.92, round.r) * smoothstep (0.05, 0.12, m.x) * smoothstep (0.85, 0.98, uInkF.x) * (1.0 - smoothstep (0.0, 0.3, uEgg)));
    // (nothing is cut off by the view's edge: towards the top and the bottom the ink, and the smoke
    // drawn as ink, give way to cloud)
    body *= 1.0 - smoothstep (0.40, 0.47, abs (p.y));
    // the mark: the buffer holds it and its mirror image; the draw keeps the true one, the other
    // half leaving behind a wipe of noise that travels with the ink as it settles into it
    float logoRedK = 0.0, eggWipe = 0.0, eggKeep = 1.0;
    if (uEgg > 0.001)
    {
        float eggK = smoothstep (0.05, 0.75, uEgg);
        float dl = logoDist (p);
        eggWipe = smoothstep (-0.2, 0.2, 1.5 * eggK - 0.25 - sv (NZ (p * 1.3 + uInkO.xy, 1.5)));
        body = mix (body, min (body, clamp (0.5 - dl / 0.05, 0.0, 1.5)), eggWipe);
        logoRedK = gLogoRed * eggWipe * (1.0 - smoothstep (-0.004, 0.012, dl));
        // (and the shade the mirrored half casts in the cloud goes with it: only the mark is in the sky)
        eggKeep = mix (1.0, 1.0 - smoothstep (-0.02, 0.14, dl), eggWipe);
    }
    vec4 wide = inkRead (m, 4.0);
    float halo = clamp (0.85 * (wide.r + smokeK * max (wide.g - inkBase - inkSlack, 0.0)), 0.0, 1.0) * eggKeep;
    // the rift is drawn here, per pixel, from the jaw as it is now: the buffer's ink follows the jaw
    // a moment late. The ink thins away into the tear over a width picked by fine noise that rises
    // with the breath, a smoky edge and never a cut-out's.
    vec4 rn = NZ (vec2 (p.x * 5.1, m.y * 3.7) + vec2 (0.0, -2.0 * uBreath.z) + uInkO.yx, max (0.5, PXL (5.1)));
    // (its torn edge is the view's own picture, not folded: a tear mirrored about the fold is a
    // pair of slits, or a small figure)
    riftField (m);
    // (only where the ink stands round it: no tear hangs in the open cloud)
    // (here crisp, there a smoky edge a few times as wide)
    float rnv = clamp (sv (rn), 0.0, 1.0);
    // (and never cut out: the ink thins away into it over a width the noise picks, so its edge is
    // torn smoke, not the edge of a hole punched in paper)
    // (and only in a body that is held: once it lets go there is no tear, or the cloud seen through
    // it would open a pale seam down the fold as the ink leaves)
    float tearSoft = (1.0 - smoothstep (-0.016 - 0.045 * rnv, 0.012, gRift)) * smoothstep (0.30, 0.55, halo) * smoothstep (0.85, 0.98, uInkF.x);
    float hole = tearSoft;
    // (the buffer's own late, coarse tear is filled in first: only this one is drawn)
    float pigment = inkRead (m, 0.7).g;
    // (and the smoke of what the mark did not keep thins behind the same wipe)
    float loose = 2.0 * uInkRule.z * max (pigment - uPig.x * max (1.0, uPig.y), 0.0) * (1.0 - smoothstep (0.40, 0.47, abs (p.y))) * (1.0 - 0.93 * eggWipe);
    float paled = smoothstep (0.15, 0.9, 1.0 - inkRead (m, 2.5).g / (uPig.x * min (1.0, uPig.y)));
    float breathLit = inkRead (m, 1.0).a, breathWide = inkRead (m, 3.5).a;
    gCarried = m - v * 2.0;
    gStretch = 0.0;
#else
    float body = inkTarget (ms);
    float halo = clamp (0.5 - gHaloD / 0.5, 0.0, 1.0);
    // (nothing is cut off by the view's edge: towards the top and the bottom the ink, its halo and
    // its smoke give way to cloud, as the buffer's do)
    float inView = 1.0 - smoothstep (0.37, 0.45, abs (p.y));
    body = mix (min (body, 0.2), body, inView);
    gFringe *= inView;
    halo *= 0.3 + 0.7 * inView;
    float logoRedK = 0.0;
    if (uEgg > 0.001)
    {
        float dl = logoDist (p);
        logoRedK = gLogoRed * smoothstep (0.05, 0.75, uEgg) * (1.0 - smoothstep (-0.004, 0.012, dl));
        gFringe *= 1.0 - smoothstep (0.05, 0.75, uEgg);
    }
#endif
#ifdef INK_BUFFER
    // (the sheen's slope from a coarser level of the buffer, by differences: the slope of one
    // magnified texel is constant across it, and would facet the gleam)
    float gt = 1.5 / uInkRes.y;
    vec2 grad = 1.7 * 0.0114 / (2.0 * gt) * vec2 (inkRead (vec2 (pf.x + gt, pf.y), 1.5).r - inkRead (vec2 (pf.x - gt, pf.y), 1.5).r,
                                                  inkRead (vec2 (pf.x, pf.y + gt), 1.5).r - inkRead (vec2 (pf.x, pf.y - gt), 1.5).r);
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
    // (and fades out at the fold, where a pattern would meet its own mirror image)
    // (CLEAN pulls the fray back to a clean edge)
    float edgeW = (1.0 - smoothstep (0.60, 0.95, body)) * smoothstep (0.0, 0.09, m.x) * (1.0 - 0.7 * uModE.x);
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
    // the modules' traces on the edge, drawn here as well: the buffer's ink is too coarse and too
    // late for a ruffle or a ripple, and inside the body it is too dense to show one
    f -= moduleTerms (vec2 (abs (gCarried.x), gCarried.y), (0.5 - body) * 0.12, 0.0) / 0.2 * 1.7 * smoothstep (0.15, 0.4, body) * (1.0 - smoothstep (0.65, 1.0, body));
#else
    float f = body + edgeW * ((detV - 0.5) * 0.24 + (fineV - 0.5) * 0.12);
#endif
    // VOCODER: strata. The fray at the edge is drawn out sideways in layers, each as torn as its
    // bands are quiet, so the outline stands in tiers. Only in the edge's own band, never across the
    // body and never on the fold (layers in the ink, never ruled lines).
    // (the layers lie along the ink as the flow has carried it, and bend with it: laid level across
    // the view they were scan lines)
    if (uModA.w > 0.002)
        f += edgeW * uModD.w * smoothstep (0.15, 0.40, body) * 0.75
           * strataFray (vec2 (m.x, m.y) - v * 2.2 + 0.10 * vec2 (detV - 0.5, fineV - 0.5) + vec2 (0.0, 0.16 * (sv (NZ (m * 0.8 + uInkO.xy + 0.29, 1.5)) - 0.5)), detV);
    float soft = smoothstep (0.35, 0.75, fine.a * 0.6 + 0.5 * (1.0 - detV));
    // (never crisper than the target's own pixels: the edge is the same at half size)
    float aa = max (0.008 + 0.045 * soft * soft + 0.05 * un, 0.75 * (abs (dFdx (f)) + abs (dFdy (f))));
    float ink = smoothstep (0.5 - aa, 0.5 + aa, f);
    float thick = smoothstep (0.5, 0.85, f);
    // OVERTONE's wisps: thin ink with a feathered edge, the body's own material
    {
        // (and only where the ink stands about it: a wisp is never left in the open cloud)
        float sp = spineInk (m, v, fineV) * uModD.w * smoothstep (0.15, 0.45, halo);
        ink = max (ink, sp);
        thick = max (thick, 0.8 * sp * sp);
        f = mix (f, max (f, 0.60), sp);
    }
    float rim = ink * (1.0 - smoothstep (0.5, 0.60, f));            // pigment pooled at the edge
    float veil = smoothstep (0.22, 0.52, f) * (1.0 - ink);          // the wet bleed outside the edge
#ifdef INK_BUFFER
    // (the ink as it stands with the tear not yet cut: the haze in front is behind neither)
    float inkWhole = ink * smoothstep (0.0, 0.5, thick);
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
    // (smoke thins away outwards and is marbled by the carried noise: thresholded to one tone it
    // was a flat grey shape with a cut edge)
    float fs = gFringe + (detV - 0.5) * 0.45 + (fineV - 0.5) * 0.2;
    float smoke = smoothstep (0.38, 1.10, fs) * (1.0 - ink) * (0.25 + 1.9 * detV * (0.25 + fine.a)) * 0.8;
    // DE-ESS: a fine spray off the body's rim about the rift (the buffer's is pigment it throws there)
    {
        vec2 rd = vec2 (m.x, m.y - uMaw.x);
        // (soft flecks of mist that thin away from the rim, never thresholded specks, which at
        // this size are a halftone screen; and none round the maker's mark, whose distance is only
        // known inside its box)
        float fleck = smoothstep (0.35, 0.85, 0.6 * sv (NZ (m * 4.5 + vec2 (0.0, -3.0 * uInkO.z), max (0.5, PXL (4.5)))) + 0.4 * fineV);
        smoke += smoothstep (0.0, 0.25, uModD.y) * 0.7 * fleck * smoothstep (0.10, 0.40, halo) * (1.0 - smoothstep (0.35, 0.60, halo))
               * exp (-dot (rd, rd) / 0.30) * smoothstep (0.04, 0.10, m.x) * (1.0 - ink) * (1.0 - smoothstep (0.0, 0.3, uEgg));
    }
    float smokeDense = 0.0;
#endif

    // ---- the cloud behind: lit billows, carried by the flow; its own picture asleep, folding
    // towards the blot's symmetry near the creature as it forms
    vec3 tone = moodTurnBy (vec3 (0.580, 0.690, 0.757) * uAtmoTint, 1.0);
    // (the lit tops lean to the hue across from the tone's, as sunlit cloud does over its own shade:
    // the frame is never one hue)
    vec3 shadow = tone * 0.70, mid = tone * 1.05, light = mix (tone, vec3 (1.0), 0.86) + 0.90 * (vec3 (dot (tone, vec3 (0.3333))) - tone);
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
    // A noise-eroded gradient, never a filled oval and never barred; some seeds and some
    // moments have none at all.
    vec2 mc = vec2 (p.x, p.y - (uMaw.x - open * 0.15));
    vec2 mr = vec2 (uMaw.y * 1.7, 0.03 + open * (0.80 + 0.45 * uInkF.y));
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
    float wisp = smoothstep (0.32, 0.60, 0.65 * sv (wn) + 0.35 * sv (bn) + 0.14 * min (breathLit, 1.0) - 0.14 * smoothstep (-0.2, 0.9, ms2.y));
    // (in patches that drift: the light never fills the tear)
    // (and broken by slow banks of the view's own noise: lit streaks one above another down the
    // fold are a dotted line)
    float lightBank = smoothstep (0.30, 0.62, sv (NZ (p * vec2 (0.8, 1.1) + vec2 (0.0, -uBreath.z) + uInkO.xy + 0.17, 2.0)));
    float glow = min (2.2 * breathLit, 1.0) * wisp * smoothstep (0.28, 0.56, sv (bn) + 0.15 * bn.a) * (0.35 + 0.65 * deep) * (0.6 + 0.4 * streak) * (0.25 + 0.75 * lightBank);
#else
    // (inside the tear's own dark only: where a rift near the body's edge is torn open to the cloud,
    // light past its end would be a lamp hung in the open sky)
    float glow = uBreath.x * mOpen * vap * (0.35 + 0.65 * deep) * exp (-0.8 * dot (ms2, ms2)) * (0.55 + 0.45 * streak) * (1.0 - smoothstep (0.65, 1.0, length (ms2 * 1.2)));
    // the breath outside it: a plume rising and drifting with the flow, ragged
    vec2 pc = vec2 (p.x, p.y - (uMaw.x + 0.35 * open + 0.06));
    float plume = exp (-(pc.x * pc.x) / 0.035 - (pc.y * pc.y) / 0.06) * smoothstep (0.35, 0.75, sv (bn) + 0.3 * bn.a);
    // (only where the ink stands round it, as the light in the tear: over a rift near the body's
    // top the plume would light the open cloud above it)
    glow += uBreath.x * uBreath.y * plume * 0.55 * F * smoothstep (0.015, 0.16, open) * (0.55 + 0.45 * streak) * smoothstep (0.15, 0.35, halo);
#endif
    // (inside the tear no smoke hangs: what shows is the cloud deep behind, shaded, lit or not)
    float tear = mOpen * (1.0 - smoothstep (0.6, 1.0, length (ms2 * vec2 (1.2, 1.2))));
    // (what shows through the tear is the cloud deep behind the body, in its shade: unlit, a dark
    // tear in the dark; never a pale mark on the fold)
#ifdef INK_BUFFER
    // (the tear's own torn shape, nearly as dark as the ink round it)
    // (dark from where the ink begins to thin: no pale line round the tear)
    // (the cloud deep behind the body, in its shade: dark, and seen to be a way through)
    // (never one flat tone: its dim banks drift with the breath)
    col *= 1.0 - min (1.0, 1.6 * tearSoft) * (0.82 - 0.30 * smoothstep (0.38, 0.70, 0.55 * sv (bn) + 0.45 * sv (wn) + 0.2 * (rnv - 0.5)));
#else
    // (only in the tear itself, whatever its shape: a thin wash of ink over it is not darkened into an oval)
    col *= 1.0 - 0.87 * tear * (1.0 - ink);
#endif
#ifdef INK_BUFFER
    // (lit vapour, warm and dim at its edges: never a white lamp)
    // (warm in every mood: breath lit pale as the cloud is a bone-white mark on the dark)
    col = mix (col, mix (light, vec3 (1.0), 0.3) * mix (vec3 (1.0), accent, 0.80 + 0.15 * (1.0 - glow)) * (0.50 + 0.40 * glow), clamp (glow, 0.0, 1.0) * (1.0 - ink));
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
    // (thin ink and smoke split to a second hue: warm, where the dense ink is cold)
    vec3 sigThin = moodTurnBy (vec3 (0.80, 0.90, 1.02), 0.33);
    // (the mark's red group is red ink)
    sigma = mix (sigma, vec3 (0.17, 0.85, 0.76), logoRedK);
    sigThin = mix (sigThin, vec3 (0.12, 0.60, 0.55), logoRedK);
#ifdef INK_BUFFER
    float marble = clamp (held.b * 1.2, -1.0, 1.0);
    // (near the fold the marbling is shaded by the view's own picture, not folded: a mark and its
    // mirror image side by side in the dark mass is a pair of eyes)
    marble *= mix (0.25 + 1.5 * clamp (sv (NZ (p * vec2 (1.4, 0.9) + uInkO.yx + vec2 (0.0, 0.3 * uInkO.z), 1.5)), 0.0, 1.0), 1.0, smoothstep (0.10, 0.40, m.x));
#else
    float marble = sv (NZ (gCarried * 0.45 + uInkO.yx, 2.0 + gStretch)) * 2.0 - 1.0;
#endif
    float veins = NZ (gCarried * 1.3 - uInkO.xy, max (1.0, PXL (1.3)) + gStretch).a;
    // some blots are one dense mass; others are marbled: part of the body is a thin wash the cloud
    // shows through, with the dense ink left as ribbons and veins in it
    float wash = smoothstep (0.30, 0.80, uInkO.w) * smoothstep (-0.25, 0.35, marble + 0.9 * (0.45 - veins));
    // (the heart of the body stays dense, to a ragged bound: the rift is a tear in the dark, never
    // a mark on a pale ground)
    // (and wide of the fold: pale ground either side of it, mirrored, is a pair of sockets)
    wash *= smoothstep (0.30, 0.62, length (vec2 (p.x, 0.5 * (p.y - uFlowC.w))) + 0.22 * marble + 0.16 * (veins - 0.5));
#ifdef INK_BUFFER
    wash *= heldShare;   // (ink the body has let go is ink, whatever the body's own marbling was)
#endif
    // ink that is still gathering, or leaving, is thin where there is little of it
    float thinEdge = mix (2.0, 0.9, smoothstep (0.0, 0.45, un));
    // (no pooled rim on ink that is gathering or leaving: a shred with a dark edge is a drawn outline)
    float A = (ink * (thinEdge + (5.8 - thinEdge) * thick) * (1.0 + 0.55 * marble * thick) + rim * 0.8 * (1.0 - smoothstep (0.0, 0.3, un))) * uInkF.w;
    float Aw = ink * (0.55 + 0.9 * thick * (1.0 - veins)) + rim * 0.5;
    // (the mark is plain ink: no marbled wash in it)
    wash *= 1.0 - smoothstep (0.05, 0.75, uEgg);
    // SHIMMER: a light second phase. Many small pale beads rise through the ink, each drawn out
    // along its way and leaving a clear track that closes behind it: an effervescence in the ink,
    // never a few round pale spots, which in a dark mass are eyes. (Light, not ink: the view's own
    // picture, never mirrored, and none near the fold.)
    float beadGlint = 0.0;
    if (uModC.z > 0.002)
    {
        float pale = 0.0;
        for (int k = 0; k < 2; ++k)
        {
            float cols = k == 0 ? 14.0 : 9.3;
            float cx = p.x * cols + 0.37 * float (k), ci = floor (cx);
            float hs = hash12 (vec2 (ci, 3.7 + 5.1 * float (k)));
            // (released under the body and rising through it to its top)
            float rise = fract (uModC.w * (1.0 + floor (hs * 3.0)) + hs * 7.0);
            float beadY = uFlowC.w - 0.36 + 0.72 * rise;
            float wob = 0.012 * sin (p.y * 23.0 + hs * 40.0);
            vec2 bd = vec2 ((fract (cx) - 0.5) / cols - wob, p.y - beadY);
            float r = 0.005 + 0.004 * hs;
            float bead = 1.0 - smoothstep (r, r * 1.8, length (bd * vec2 (1.0, 0.35)));
            // (the track wanders as the bead did, and fades the further behind it is)
            float track = (1.0 - smoothstep (0.0015, 0.005, abs (bd.x))) * smoothstep (-0.16, 0.0, bd.y) * step (bd.y, 0.0)
                        * (0.4 + 0.6 * NZ (vec2 (p.x * 3.0, p.y * 9.0) + hs, 1.0).r);
            pale = max (pale, max (bead, 0.6 * track) * smoothstep (0.02, 0.10, rise) * (1.0 - smoothstep (0.85, 0.98, rise)) * step (0.45, fract (hs * 13.0)));
        }
        // (in the dense ink only: at the thin edge a bead would move the outline one side only)
        pale *= smoothstep (0.0, 0.3, uModC.z) * smoothstep (0.10, 0.20, abs (p.x)) * smoothstep (0.4, 0.9, thick);
        // (thinned, and lit below: a glint in the dark, never a hole through to the cloud)
        A *= 1.0 - 0.35 * pale;
        Aw *= 1.0 - 0.3 * pale;
        beadGlint = pale;
    }
    // ENSEMBLE: pale echoes of the outline, stepped outward
    float echo = 0.0;
    if (uModC.y > 0.002)
    {
#ifdef INK_BUFFER
        vec2 ec = vec2 (0.0, uFlowC.w);
        vec4 e1 = inkRead (ec + (m - ec) / (1.0 + uModC.y), 1.0), e2 = inkRead (ec + (m - ec) / (1.0 + 2.0 * uModC.y), 1.5);
        echo = smoothstep (0.30, 0.70, e1.r + smokeK * max (e1.g - inkBase - inkSlack, 0.0)) + 0.5 * smoothstep (0.30, 0.70, e2.r + smokeK * max (e2.g - inkBase - inkSlack, 0.0));
#else
        float es = 0.45 * uModC.y;
        echo = (1.0 - smoothstep (es - 0.02, es + 0.02, gHaloD)) + 0.5 * (1.0 - smoothstep (2.0 * es - 0.02, 2.0 * es + 0.02, gHaloD));
#endif
        // (in drifting patches, like breath on glass: never whole stepped copies of the outline)
        // (folded as the ink is: they are its echoes)
        echo *= (1.0 - ink) * smoothstep (0.0, 0.03, uModC.y) * smoothstep (0.25, 0.65, sv (NZ (m * 1.6 + uInkO.yx * 2.0 + vec2 (0.0, uInkO.z), 1.5)));
    }
    col *= exp (-sigThin * 0.35 * echo);
    col *= exp (-sigma * mix (A, 0.0, wash)) * exp (-sigThin * (veil * 0.75 + 1.5 * Aw * wash)) * exp (-mix (sigThin, sigma, smokeDense) * smoke * SMOKE_DEPTH);
    // (SHIMMER's beads, lit from below in the dark ink)
    col += light * mix (vec3 (1.0), vec3 (0.85, 0.95, 1.05), 0.5) * 0.15 * beadGlint * ink;

    // ---- wet sheen on the thick ink (the light is above and to the left): one broad soft band
    vec2 n2 = grad * 5.0 + gv (det) * 0.9 * 0.004;
    float nl2 = length (n2);
    float spec = pow (clamp (0.5 - 0.5 * dot (n2 / (nl2 + 1.0e-4), L), 0.0, 1.0), 4.0) * smoothstep (0.12, 0.6, nl2);
    // (no gleam on the rift's rim: a ring of light round a dark opening is an eye)
    // (the gleam is the light's own picture, not folded, and faint near the fold: a gleam that is
    // one of a mirrored pair, on two round masses side by side, is a pair of eyes)
    float gleam = smoothstep (0.42, 0.62, sv (NZ (p * vec2 (0.50, 0.85) + uInkO.yx * 2.0 + vec2 (uInkO.z, 0.0), 2.0)))
                  * smoothstep (0.18, 0.55, m.x);
    col += light * spec * gleam * ink * (0.13 + 0.08 * roar) * smoothstep (0.9, 1.8, length (ms2)) / (1.0 + 2.0 * gStretch);
    col += light * thick * 0.035 * smoothstep (0.45, 0.75, detV) * smoothstep (0.18, 0.55, m.x);

    // ---- the cloud in front: optical depths with banks and gaps, never a flat layer. The body
    // parts it where it is deepest and the roar thins it; its banks drift across the rest of the
    // creature and may hide it, and the ink behind shades them.
    // (how far the body parts the cloud in front of itself is the blot's own: some stand clear,
    // some are half hidden)
    float clearing = 0.95 - 0.40 * clamp ((uCloudK.y - 0.55) / 0.8, 0.0, 1.0);
    float thin = (1.0 - uCloudK.w) * (1.15 - 0.3 * F);
#ifdef INK_BUFFER
    // (deepest: the ink over a wide round. Its heart stands clear; limbs and edges are left to the banks.)
    float deepInk = smoothstep (0.35, 0.80, round.r + smokeK * max (round.g - inkBase - inkSlack, 0.0));
    // (ink let go still parts the cloud while it hangs dense: the banks close over it as it thins,
    // not the moment the hold gives)
    deepInk = max (deepInk, 0.8 * smoothstep (0.15, 0.7, loose)) * eggKeep;
    // (the tear is in the body's shade and behind the same parted cloud as the ink round it)
    float behind = max (clamp (inkRead (m, 2.5).r, 0.0, 1.0) * eggKeep, hole);
    float inkHere = max (inkWhole, hole);
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
    // CHAMBER: the room is the cloud. The near layer thickens into walls at the edges of the view,
    // the room's ringing sends slow bands outward through its banks, and the body's shadow in it
    // grows long and soft.
    float wall = uChamber.y * smoothstep (0.70, 1.65, abs (p.x) + 0.30 * (nearC.x - 0.5));
    float rC = length (p - vec2 (0.0, uFlowC.w)) + 0.10 * (nearC.x - 0.5);
    float bands = uChamber.x * sin (rC * 7.5 - 6.2832 * uChamber.z) * smoothstep (0.15, 0.5, rC);
    float tauN = uCloudK.y * 6.0 * fd * thin * (1.0 - part) * (1.0 + 0.9 * bands) + uCloudK.z * (1.0 - 0.9 * inkHere) + (1.2 + 3.0 * fd) * wall;
    float aN = 1.0 - exp (-tauN);
    float ml = clamp (0.62 + dot (nearC.yz + gv (f3) * 1.5 * 0.12, L) * 0.30, 0.0, 1.0);
#ifdef INK_BUFFER
    float longShade = uChamber.w * clamp (inkRead (vec2 (m.x * 0.8, p.y + 0.12), 5.0).r, 0.0, 1.0);
#else
    float longShade = uChamber.w * smoothstep (0.0, 0.6, halo);
#endif
    vec3 mistCol = mix (mid, light, ml) * (1.0 - 0.25 * smoothstep (0.2, 0.9, halo)) * (1.0 - 0.22 * behind) * (1.0 - 0.30 * longShade) * (1.0 - 0.22 * wall);
    col = mix (col, mistCol, aN);
    // the breath scattered in the near cloud
    vec2 sc = mc / vec2 (0.27, 0.19);
    // (in the cloud, where there is cloud to scatter it: never a stain on the bare ink)
    // (and ragged with it: a round stain of light about the rift would be a lamp's)
    col += accent * exp (-dot (sc, sc)) * smoothstep (0.40, 0.72, sv (bn) + 0.3 * bn.a) * uBreath.x * mOpen * (0.05 + 0.12 * uBreath.y) * (0.15 + 0.85 * aN);
    // and drifting out in front of the body: ragged lit vapour the flow carries up and away
    vec2 bc = vec2 (p.x, p.y - uMaw.x - 0.04 - 0.10 * uBreath.y) + (bn.gb - vec2 (0.5)) * 0.5;
    float reachB = dot (bc / vec2 (0.20 + 0.10 * uBreath.y, 0.14 + 0.08 * uBreath.y), bc / vec2 (0.20 + 0.10 * uBreath.y, 0.14 + 0.08 * uBreath.y));
#ifdef INK_BUFFER
    // (ragged patches of it, however much breath there is: never a round stain over the ink)
    float vapour = smoothstep (0.46, 0.74, 0.55 * sv (wn) + 0.45 * sv (bn) + 0.10 * min (breathWide, 1.0)) * clamp (4.0 * breathWide, 0.0, 1.0);
#else
    // (before the body, not in the open cloud past its edge)
    float vapour = smoothstep (0.42, 0.90, sv (bn) + 0.35 * bn.a + 0.15 - reachB) * uBreath.x * smoothstep (0.015, 0.16, open) * F * smoothstep (0.30, 0.50, halo);
#endif
    // (streaked: brighter and dimmer along the wisps, never a flat patch)
    vapour *= 0.55 + 0.45 * streak;
    // (light added to what is there, the ink included: lit vapour in front of the dark, never a tan stain on it)
    // (thinly over the dense ink itself: lit vapour drifting in front of it, never the ink paled away)
    col = vec3 (1.0) - (vec3 (1.0) - col) * (vec3 (1.0) - 0.70 * vapour * (1.0 - 0.6 * ink * smoothstep (0.3, 0.8, thick)) * light * mix (vec3 (1.0), accent, 0.7));

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
    if (program == Program::copy)
        return c_copy;
    if (program == Program::probe)
        return c_probe;
    // the noise is read at an explicit level of detail where the context can (every core context;
    // older ones through the extension the caller enables), so a jump in the lookup position never
    // picks a wrong mip
    s += textureLod ? "#define NZ(uv, lod) texture2DLod (uNoise, uv, lod)\n" : "#define NZ(uv, lod) texture2D (uNoise, uv)\n";
    if (program != Program::singlePass)
        s += "#define INK_BUFFER 1\n#define IT(uv, lod) texture2DLod (uInkTex, uv, lod)\n";
    if (program == Program::sim)
        s += "#define INK_SIM 1\n";
    s += c_inkCommon;
    if (program != Program::singlePass)
        s += c_inkBuffer;
    // (the buffer's step and the draws share everything above: the tiers cannot drift apart)
    s += program == Program::sim ? c_inkSim : c_inkSingle;
    return s;
}

const char* name (Program program)
{
    static const char* const names[] = { "single pass", "ink step", "composite", "copy", "probe" };
    const auto i = static_cast<int> (program);
    return i >= 0 && i < static_cast<int> (Program::numPrograms) ? names[i] : "?";
}
}
