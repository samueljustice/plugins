# v3 research: a real-time OpenGL pirate ship that is also the instrument

Status: research and design only, no source edited.
Target: JUCE 8.0.8 (the copy in `../SDK/JUCE`), `juce::OpenGLContext`, OpenGL 3.2 core / GLSL 150, macOS (Retina, Apple Silicon) and Windows. Design size 1280 x 800.
Hard rule: no downloaded assets. Every mesh, texture and colour comes from code.

---

## 0. The idea in one paragraph

The window is a dusk sea. A chunky, toon-shaded, low-poly pirate ship rides Gerstner swells **that come from the sound**. Long swells follow the low bands, chop follows the highs, and the swell height follows overall energy. The camera is the listener, a little above the waterline. **Every whoosh is a cannon shot.** The muzzle flashes, smoke puffs out, and the cannonball's flight past the camera *is* the doppler flight: it comes in from the aim direction, is loudest and closest at the flight's peak, then drops into the sea with a splash. Users get four direct handles and nothing else in the scene: **drag the cannon** to aim (flight path), **drag the sails** up or down (energy / size), **click the ship or press Space** to fire, and **drop audio on the ship** to load cargo. Each dropped sample becomes a barrel on deck, labelled with the material the classifier detected ("Cloth", "Fire", "Metal"...). That label is also the name of the dynamic track. Everything else stays under the hood.

This replaces the current `SeaVisualiser` (a `juce::Graphics` spectrum waterfall drawn over a GL-composited component) with a true 3D scene that owns its own GL pipeline.

---

## 1. What exists today (codebase notes)

- `Source/PluginEditor.cpp:594` attaches one `juce::OpenGLContext m_gl` to the whole editor with `setComponentPaintingEnabled(true)`. That means GL only composites JUCE's 2D painting. There is no `OpenGLRenderer` and no shaders, and no GL version is requested, so you get the legacy/compatibility profile by default.
- `Source/SeaVisualiser.*` does a 2048-point FFT on a 30-60 Hz `juce::Timer` (message thread), keeps 96 bands x 42 rows, and paints a perspective waterfall plus "cannonball" comets from `WhooshEngine::FlightView`.
- `Source/WhooshEngine.h:68` already exposes exactly what the scene needs, lock-free. For each of `c_maxFlights = 4` it gives `{active, env 0..1, x -1..1, closeness 0..1, age 0..1}`, plus `getEnergy()` and `getPeakPhase()`. This is the contract between the audio engine and the ship.
- `Source/LogoOutline.cpp:227-234` draws the Sweet Justice logo with `glBegin/glEnd/glColor4f/glVertex2f`. **That is fixed-function GL and will not run in a 3.2 core context.** It must be ported to a VBO + tiny shader (or kept on the `juce::Graphics` overlay path) before core profile is switched on. Its `Mat4 frustum()` / projection helpers can be reused.
- `Source/SampleClassifier.*` and `Source/Cargo.*` already classify dropped samples. The scene only needs `{trackId, label, colour}` for each loaded sample to spawn and name a barrel.
- `Source/FlightPad.*` (draw / dice "Throw it") is today a separate 2D pad. In v3 its path model moves into the 3D scene: the cannon's aim *is* the path, and the dice becomes "roll the dice" on deck (a random aim + arc).

---

## 2. Interaction design: the scene is the control surface

Research on simple interfaces (see `docs/experiments/01-simplicity-ux.md`) points to one primary action, direct manipulation of the thing itself, and progressive disclosure. In the ship, each control is an object you can see and grab:

| Object in the scene | Gesture | Parameter(s) under the hood | Feedback |
|---|---|---|---|
| **Ship hull / big cannon** | click, or Space | `trigger(velocity)`. Velocity comes from click y, or a fixed value | muzzle flash, recoil, ball flies |
| **Cannon barrel** (bow swivel gun aimed at camera) | drag left/right = azimuth, up/down = elevation | flight direction (L-R / R-L), distance (how close it passes), length (arc height = time in air) | a dotted aim arc is drawn live across the sea, the same path the flight pad used to draw |
| **Sails** | drag the mainsail up (furled) or down (full) | Energy / Size macro (rubber-banded: drag returns to the preset value on double-click) | sails billow more, ship heels, wake grows, wind lines streak |
| **Flag at the masthead** | drag left/right | Swirl / character macro (flutter speed = modulation rate) | flag flaps faster or slower |
| **Barrels on deck** (one per dropped sample) | drop file on ship = new barrel; drag barrel overboard = remove; click barrel = solo/preview; scroll on barrel = level | dynamic cargo tracks, auto-named from the classifier | barrel shows a painted label, e.g. "WHOOSH-CLOTH", tinted by category |
| **Dice on deck** | click | "Throw it": random aim + arc | dice tumbles (procedural cube), then the aim arc animates to its new spot |
| **Moon** | click | open the "More" drawer (2D overlay) for mastering, tape and so on | moon glows |

Rules that keep it simple:
- **No more than these 6 grabbable objects.** Hover outlines them in amber (the brand accent) and shows a one-line tooltip in the status bar.
- Every gesture is rubber-banded. Double-click resets to the default. Alt-drag gives fine control.
- 2D overlays drawn with `juce::Graphics` are limited to: top bar (preset name, tape "record last 30 s" button, Swash button), barrel labels (drawn into a texture, see 4.6), and the More drawer.

Picking is done on the **message thread** in `mouseDown/mouseDrag`. Unproject the mouse ray with the same view/projection matrices the GL thread used (copy them into an atomic-guarded snapshot each frame). Then test it against bounding spheres/capsules for ship, cannon, sails, flag, barrels and dice, and against the y=0 plane for aiming. No GPU readback is needed.

---

## 3. Art direction (Sweet Justice, not "posh")

- Palette from `Theme.h`: navy (sky top / deep water), teal (water mid-band, shadows), rose (sky at the horizon at dusk, sail shadow tint, flag), amber (lanterns, muzzle flash, moon rim, hover). Hull is a warm brown pushed toward rose. Sails are off-white with a teal shadow band.
- **Cel shading** with 2-3 hard bands, a rim light and thick dark-navy outlines (not black) gives the playful Wind Waker / cartoon look. Wind Waker's cel shading was chosen to make objects read clearly, which is exactly what a control surface needs ([Wikipedia: Cel shading](https://en.wikipedia.org/wiki/Cel_shading); [Nathan Gordon, Wind Waker Graphics Analysis](https://medium.com/@gordonnl/wind-waker-graphics-analysis-a0b575a31127)).
- Proportions are toy-like: a short, fat hull (length:beam ~ 2.6:1, a real galleon is ~3.5:1), oversized cannon, oversized flag with the grenade + cherry Jolly Roger.
- Night/dusk sky with a big moon, a few stars, and the Sweet Justice logo tracing itself in the sky when idle (keep this existing gag).

---

## 4. Procedural ship

All geometry is built once at `newOpenGLContextCreated()` (or better, on the message thread at editor construction into CPU vectors, then uploaded on the GL thread). The whole ship is about 3-6k triangles.

### 4.1 Hull: lofted cross-sections
Represent the hull as stations along the length `u in [0,1]` (stern to bow). Each station is a half cross-section, mirrored for the other side.

```
beam(u)   = B * pow(sin(pi * mix(0.08, 1.0, u)), 0.6)   // fat middle, tapering bow, blunt transom
depth(u)  = D * (1 - 0.35 * pow(u, 3))                    // shallower toward bow
sheer(u)  = S0 + S1 * pow(2u-1, 2) + bowRise * smoothstep(0.8, 1, u) + sternRise * smoothstep(0.25, 0, u)
section(u, t), t in [0,1] keel -> gunwale:
   y = -depth(u) * (1 - t) + sheer(u) * t
   x =  beam(u) * pow(t, 0.45)                            // rounded bilge; exponent < 1 = round, > 1 = V
```

- 14-18 stations x 8 points per side is enough for low-poly. Use **flat shading** (duplicate vertices per face, face normal), because faceted reads as "toon wood". The outline pass uses a separate smoothed normal (see 6.2).
- Planking: the vertex colour alternates by `floor(t * 7)` (two browns). A stripe at the gun deck is painted rose, with square gun ports cut as darker quads (just colour, no actual holes).
- Deck = cap polygon at the sheer line, triangulated as a strip between the port and starboard gunwale points.
- Stern castle and forecastle are boxes extruded from the deck, slightly tapered. The bowsprit is a cone.
- A railing is a line of thin boxes (or `GL_LINES` with the outline colour, which is cheaper and very cartoon).

### 4.2 Masts, yards, rigging
- Masts are 6-sided tapered cylinders (2-3 masts), and yards are horizontal tapered cylinders. The crow's nest is a short open cylinder.
- Rigging is a `GL_LINES` batch from mast tops to gunwale points, in outline colour. It costs nothing and sells "ship" instantly. Line width is not reliable in core profile (only width 1 is guaranteed), so either accept 1px*scale lines or build thin quads.

### 4.3 Sails: cloth-ish mesh driven by wind
Each sail is a grid (e.g. 12 x 10) in sail-space `(s, v)`, s across and v down, hung from a yard. Use a vertex shader with no CPU simulation:

```glsl
// billow: belly bulges along the sail normal, strongest centre-low, pinned at yard and corners
float belly = sin(PI * s) * pow(sin(PI * clamp(v * 0.9 + 0.1, 0.0, 1.0)), 0.8);
float gust  = 0.08 * sin(uTime * 3.1 + s * 5.0 + v * 2.0) * uWind;   // flutter
pos += sailNormal * (uWind * uBillow * belly + gust * belly);
pos.y += uFurl * v * v * sailHeight;   // furling: pull rows up toward the yard
```

- `uWind` = smoothed energy (0..1). `uFurl` = 1 - sails control. The normal is recomputed analytically from the derivative of `belly` (or a cheap finite difference in the shader), so the cel bands slide across the sail as it fills. That moving band is what makes it look alive.
- The same approach is used in shipping games: a vertex shader on a flat mesh scaled by a global wind value, sometimes with baked detail ([moddb, Vertex shader fun](https://www.moddb.com/news/vertex-shader-fun-august-15-2019); [Lukas Stratmann, Sails of War](https://lukasstratmann.artstation.com/projects/LeR1k5)). We skip the baked part.

### 4.4 Flag
A 16 x 6 ribbon pinned at `s=0`. The travelling wave grows with distance from the pole:
`pos.z += s * A * sin(k*s - w*uTime) ; pos.y += s * 0.3*A * sin(1.7*k*s - 1.3*w*uTime)`, where `w` = flag macro.
The texture is the grenade + cherry Jolly Roger drawn **with `juce::Graphics` into a `juce::Image`** (from `Resources/sj-logo.svg` already in BinaryData, or `LogoOutline` paths). It is uploaded once as `juce::OpenGLTexture`. That is code-made, not a downloaded asset.

### 4.5 Cannons
Build a lathe (surface of revolution) from a 2D profile polyline: muzzle swell, chase, reinforce rings, cascabel knob. Use 10 segments around. Carriage = boxes plus 4 cylinder wheels. One "hero" swivel cannon on the bow faces the camera and is the aim handle. Smaller broadside cannons are instanced (same VBO, different model matrix) through the gun ports and fire alternately for visual variety.

### 4.6 Barrels (cargo)
A lathe again (bulged cylinder) with two dark hoop rings. Each barrel gets a label: render "CLOTH" (Lilita One or the theme font) with `juce::Graphics` into one shared **atlas texture** (e.g. 1024 x 256, 8 slots), and sample it on a quad in front of the barrel. Re-render a slot only when the cargo list changes. The category colour tints the barrel band.

### 4.7 Mesh builder
A single `MeshBuilder` with `addQuad`, `addLathe(profile, segments)`, `addLoft(stations)`, `addBox`, `addCylinder` and `transform(mat4)` emits into `std::vector<Vertex>`. Output is three static meshes: `shipStatic` (hull, deck, masts, cannons, barrels), `sails` (one draw, sail id in a vertex attribute), `flag`.

---

## 5. Ocean

### 5.1 Geometry
- A **camera-centred radial grid** (more rings near the camera, e.g. 64 rings x 96 spokes ~ 12k triangles). It extends to ~400 m and fades into fog. Alternative: a 160 x 160 regular grid of 1 m cells near the camera plus a coarse skirt to the horizon. The radial grid gives LOD for free.
- The vertex format is just `vec2 xz`. Everything else is computed in the shader.

### 5.2 Gerstner waves (in the vertex shader)
Use the standard trochoidal form ([GPU Gems ch.1, Finch](https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models); [Catlike Coding, Waves](https://catlikecoding.com/unity/tutorials/flow/waves/)):

```
k = 2*pi / wavelength,   c = sqrt(9.8 / k),   a = steepness / k
f = k * (dot(D, xz) - c * t)
P.x += D.x * a * cos f ;  P.y += a * sin f ;  P.z += D.y * a * cos f
T += (-D.x*D.x*s*sin f,  D.x*s*cos f, -D.x*D.y*s*sin f)
B += (-D.x*D.y*s*sin f,  D.y*s*cos f, -D.y*D.y*s*sin f)   // start T=(1,0,0), B=(0,0,1)
N = normalize(cross(B, T))
```
Catlike's note matters: the steepness of all waves must sum to 1 or less, or crests loop. Their multi-wave code also starts T/B at zero, which gives wrong normals. Start from (1,0,0)/(0,0,1).

**Audio mapping** (the "sea is the sound" part). Use 8 waves in a uniform array `uWaves[8]` = `(dirX, dirZ, steepness, wavelength)`:
- waves 0-1: long swell (wavelength 40-60 m), steepness = 0.10 + 0.20 * energy
- waves 2-4: mid (8-20 m), steepness from low-mid spectrum bands (a 96-band FFT folded to 3 values)
- waves 5-7: chop (1.5-5 m), steepness from high bands. Their direction rotates slowly with wind.
- On each flight peak (`env` crossing its max): add a transient "kick" to waves 2-4 that decays over ~0.5 s, so loud whooshes visibly shove the sea.
- Normalise so the sum of steepness is at most 0.9. Smooth every value with a one-pole filter (tau ~120 ms up, 400 ms down) to avoid jitter.

The **ship bobs on the same waves**. Evaluate the identical Gerstner sum in C++ at 4 hull points (bow, stern, port, starboard) each frame. That gives heave = mean height, pitch = atan(bow - stern), roll = atan(port - starboard). Smooth these values. Put the C++ and GLSL versions of the wave function side by side in one header comment so they stay in sync. Cost: 4 points x 8 waves, trivial.

### 5.3 Toon water shading (fragment)
- **Banded colour**: compute `h = (P.y - minH) / range` and `ndl = dot(N, L)`. Pick 3 colours (deep navy, teal, light teal) with `step()`, not smooth lerp. Adding a step on ripple edges is the usual trick for "toon" water ([Unity discussion on stepped ripples](https://discussions.unity.com/t/how-to-color-a-complex-ocean-waves-shader-with-vertex-displacement/1547370)).
- **Crest foam**: Gerstner squash gives a Jacobian-like term for free. `J = 1 - sum(s_i * sin f_i)` is small where crests pinch. Foam where `J < threshold + noise`. Use a cheap procedural value-noise or Worley noise in the shader (no textures). Quantise to white or nothing for the cartoon look.
- **Wind Waker style foam lines**: Wind Waker's sea is a flat tinted plane with scrolling white curl patterns. A Godot recreation produces the foam fully procedurally, with no textures ([NekotoArts, Wind Waker water, no textures](https://godotshaders.com/shader/wind-waker-water-no-textures-needed/); [Toon water, no textures](https://godotshaders.com/shader/toon-style-3d-water-shader-no-textures-needed/)). Port that idea: Voronoi cell edges, `step(edgeDist, 0.04)`, domain-warped by time. Fade it out with distance to avoid aliasing.
- **Intersection foam around the hull and splashes**: the classic depth-buffer approach (Roystan) needs a depth texture of the scene ([Roystan toon water, source](https://github.com/IronWarrior/ToonWaterShader); [three.js forum: toon water with depth fog and intersection foam](https://discourse.threejs.org/t/toon-water-shader-with-depth-based-fog-and-intersection-foam/35978)). It is cheaper and more controllable to fake it:
  - **Hull ring**: pass the ship's waterline as an ellipse (centre, axes, yaw). Foam where `abs(ellipseDist) < width`, with noise breakup.
  - **Wake**: foam in a V behind the stern, with width and length scaled by sails/energy.
  - **Splash rings**: `uniform vec4 uRings[8]` (x, z, startTime, strength). Expanding foam rings where cannonballs hit. The ring also displaces height slightly: `y += strength * exp(-age) * sin(10*(r - age*6))`.
- **Moon glint**: a stylised specular. Compute `pow(max(dot(reflect(-V,N), Lmoon),0), 60)`, step it to a hard white blob, and stretch it vertically by scaling the reflection x component. This gives the classic "moon road" in amber/white.
- **Fog**: exponential-squared toward the horizon colour (the sky's bottom colour, rose at dusk). Fog hides the grid edge and makes the horizon read.

### 5.4 Sky
A fullscreen triangle drawn first, with depth write off. It reconstructs a view ray from the inverse view-projection and computes:
- a vertical gradient navy, then teal, then rose at the horizon (3 stops, slight banding allowed for the cartoon feel),
- a moon disc: `smoothstep` on angle to the moon direction, an amber rim, and two darker "craters" from offset discs,
- stars: hash the ray direction into cells and light a few cells, with twinkle from `sin(time + hash)`. They fade near the horizon.
- The idle Sweet Justice logo trace (existing `LogoOutline`) is drawn here after porting to VBO lines.

three.js's ocean example shows the same split (Water + Sky + sun-direction uniform), but its look is realistic and it expects `NoToneMapping` ([three.js webgl_shaders_ocean](https://github.com/mrdoob/three.js/blob/dev/examples/webgl_shaders_ocean.html); [black water pitfall](https://discourse.threejs.org/t/when-copying-the-code-for-water-ocean-object-examples-the-water-is-rendered-black/24935)).

---

## 6. Lighting and outlines

### 6.1 Toon shader for ship / props
Based on Roystan's toon shader: a single directional light, hard bands, specular blob, rim light only on lit side ([gamedev.net mirror of the Roystan tutorial](https://click.gamedev.net/tutorials/programming/graphics/toon-shader-using-unity-r5095); [source](https://awesome.ecosyste.ms/projects/github.com%2FIronWarrior%2FUnityToonShader)).

```glsl
float ndl   = dot(N, uLightDir);
float band  = ndl > 0.35 ? 1.0 : (ndl > -0.1 ? 0.62 : 0.38);       // 3 bands
vec3  shade = mix(uShadowTint, vec3(1.0), band);                   // shadow is teal-tinted, not grey
float rim   = (1.0 - max(dot(V, N), 0.0));
rim         = smoothstep(0.70, 0.72, rim * pow(max(ndl, 0.0), 0.1));
vec3  col   = albedo * shade * uLightCol + rim * uRimCol + uAmbient * albedo;
col += uFlash * uFlashCol * max(dot(N, normalize(uFlashPos - wpos)), 0.0) / (1.0 + dist2);  // muzzle flash point light
col  = mix(col, uFogCol, fogFactor);
```
Lights: the moon (cool, from behind-left, gives rim), a warm fill from the stern lanterns (amber), and a transient muzzle-flash point light.

### 6.2 Outlines: inverted hull
Draw each outlined mesh a second time with `glCullFace(GL_FRONT)`, vertices pushed out along a **smoothed** normal, flat dark-navy colour. Push in clip space so the line width stays constant on screen:

```glsl
vec4 clip = uViewProj * uModel * vec4(aPos, 1.0);
vec3 nClip = mat3(uViewProj * uModel) * aSmoothNormal;
clip.xy += normalize(nClip.xy) / uViewport * uOutlinePx * 2.0 * clip.w;
```
Split normals at hard edges cause gaps. The fix is to store a separate averaged normal per position (we already have flat normals for shading, so the smooth normal is a second attribute) ([BNPR inverse hull](https://bnpr.gitbook.io/bnpr/outline/inverse-hull-method); [Delt06 URP toon outline wiki](https://github.com/Delt06/urp-toon-shader/wiki/Outline); [UE forum on gaps](https://forums.unrealengine.com/t/best-way-to-fix-gaps-in-toon-outlines-when-using-inverted-hull-method/1319625)).
The outline doubles the vertex cost of the ship only, which is cheap. Do not outline the ocean. A hover highlight is the same pass with amber colour and a larger width for the hovered object.

### 6.3 Anti-aliasing
`OpenGLPixelFormat pf; pf.multisamplingLevel = 4; m_gl.setPixelFormat(pf); m_gl.setMultisamplingEnabled(true);` must be set **before** `attachTo`. 4x MSAA on a Retina framebuffer is fine on M1. On weak Windows iGPUs, drop to 2x or none and rely on the outlines. Thin rigging lines will alias without MSAA.

---

## 7. Cannon fire = the doppler flight

The engine already exposes, per flight, `x` (-1..1 lateral, 0 at peak), `closeness` (0..1), `age` (0..1), `env`. **Do not invent a separate visual trajectory.** Map the engine's numbers directly so picture and sound cannot drift.

```
// listener (camera) at origin, looking -Z toward the ship at z = -Zs
P(age) = mix(muzzlePos, impactPos, age)                       // base line
       + lateralAxis * x * halfSpan                           // engine's lateral position
       + forwardAxis * (1 - closeness) * passDistance         // how far from camera it passes
       + up * arcHeight * 4 * age * (1 - age)                 // parabolic arc
```
- `muzzlePos` = the hero cannon's barrel tip. `impactPos` = behind and beside the camera (fly-by) or in the water on the near side (splash), based on the aim's elevation. When the aim says "past the camera", the ball passes within ~1-2 m of the lens at `age ~ 0.5`. That is the doppler peak, so add a tiny camera shake (2-4 px, 120 ms) and a radial whoosh streak.
- **Muzzle flash** (age 0): an additive star billboard (procedural: `pow(1-r,3)` plus 6 rays from `abs(cos(6*atan))`) lasting 80 ms, a point light on the toon shader, and a 1-frame screen brightness bump. The barrel recoils (model matrix, spring back over 300 ms).
- **Smoke**: a CPU particle pool (512 max). Spawn 20-40 puffs per shot along the barrel axis with random cone velocity, drag and buoyancy. Draw them as camera-facing billboards with a **toon-stepped soft disc** (`step(0.5, noise + 1 - r)`) so they look like cartoon cotton balls, not realistic smoke. Puffs darken from amber-lit to grey-teal as they age.
- **Cannonball**: a sphere (icosphere level 1, outlined) plus a **trail ribbon** of the last 24 positions, camera-facing, tapered, fading with alpha. On the close fly-by, stretch the ball along its velocity (`scale += |v|*dt` in screen space) for a speed-blur cartoon smear.
- **Splash** (if it lands in water): spawn a ring in `uRings`, 30 droplet particles (white-teal, gravity) and a short-lived vertical "plume" cone that scales up then collapses.
- Mapping `env` to ball glow keeps the ball's brightness tied to loudness. A 4-flight max = 4 balls, matching `c_maxFlights`.
- Detection of a new flight: `active` goes false to true, or `age` resets to below its previous value. This is done on the GL thread when reading `FlightView`.

Aim arc preview: when dragging the cannon, draw the trajectory for a hypothetical flight (age 0..1 sampled at 32 points) as a dotted amber line on the sea, with a small target ring where it lands. This replaces the 2D flight pad but uses the same path parameters.

---

## 8. Camera

- Fixed composition, slightly low: eye at (0, 2.2, 0), looking at the ship ~28 m away, FOV ~38 degrees vertical. The ship sits in the upper-middle third, the sea fills the bottom. This keeps every grabbable object large and still, which matters for usability.
- Subtle life: the camera rides a heavily damped version of the wave height at its own position (~20% amplitude), plus a 0.5-degree slow drift. Add an "Excitement" zoom: on big energy the FOV narrows by 2 degrees. Never move the ship out from under the cursor. While a drag is active, freeze camera motion entirely.
- Optional idle mode (no interaction for 30 s): a slow orbit, returning on mouse move.

---

## 9. Performance budget (60 fps at 1280 x 800 logical, Retina = 2560 x 1600 physical)

| Item | Triangles | Draw calls | Notes |
|---|---|---|---|
| Sky (fullscreen tri) | 1 | 1 | fill: whole screen, cheap ALU |
| Ocean radial grid | ~12k | 1 | 8 Gerstner waves in VS = ~100 ALU ops/vertex; foam noise in FS is the main fill cost |
| Ship static + outline | ~5k x 2 | 2 | |
| Sails + outline | ~1.2k x 2 | 2 | |
| Flag | ~200 | 1 | |
| Broadside cannons instanced | ~1k | 1 | |
| Barrels + labels | <2k | 2 | |
| Particles (instanced quads) | <=1k quads | 1-2 | additive + alpha passes |
| Balls + trails | small | 2 | |
| Rigging lines | ~60 lines | 1 | |
| JUCE 2D overlay | n/a | ~10-30 | via `setComponentPaintingEnabled(true)` |
| **Total** | **~25k** | **~25-45** | Far below any 2020+ iGPU's limits |

- **GPU**: the cost that matters is fragment fill at 4 Mpx x MSAA. Keep the ocean fragment shader under ~60 instructions: 1 Voronoi evaluation (3x3 cells), 1 value noise, no loops over waves (the wave sum is in the VS, and the foam term J is passed down as a varying). Expect around 2-4 ms on M1 at Retina and around 6-9 ms on an Intel UHD 620 at 1080p. Add a **quality switch**: if a rolling average frame time stays over 14 ms, render the 3D pass at 0.75 scale into an FBO and upscale. JUCE's 2D components stay sharp because they render separately at full scale.
- **CPU, GL thread** (`renderOpenGL`): about 0.5 ms or less. That covers reading atomics, smoothing about 20 values, 4-point C++ Gerstner for ship pose, updating particles (512 x a few flops), uploading one dynamic VBO (particles + trails, at most 40 KB) with `glBufferData(..., GL_STREAM_DRAW)` orphaning, and setting about 30 uniforms.
- **CPU, message thread**: picking, drag handling, label atlas redraws (only on cargo change), and the FFT. Move the FFT off the message thread: the audio thread pushes samples into a lock-free FIFO (existing `m_scopeWrite` style ring), and the **GL thread** pulls 2048 samples and runs `juce::dsp::FFT` each frame. That is about 30 us, fine. Then the message thread does nothing per frame except JUCE's own component painting.
- **Audio thread**: untouched. It only writes the existing atomics (`m_viewData`, `m_energyView`) and the scope ring. Never take a lock that the GL thread holds.
- **Frame pacing**: `setContinuousRepainting(true)` + `setSwapInterval(1)` (vsync) is simplest. Windows forum reports show continuous repainting can cost 5-8% of a core in `wglMakeCurrent`/swap. One workaround is continuous off, swap interval 0, and a vblank-driven `triggerRepaint()` ([JUCE forum: continuous repaint hogs a core](https://forum.juce.com/t/br-fr-opengl-context-with-continuous-repaint-hogs-one-core-100/61253); [OpenGL using a lot of CPU](https://forum.juce.com/t/br-opengl-is-using-a-lot-of-cpu/50288); [low FPS in plugins](https://forum.juce.com/t/low-fps-with-opengl-rendering-in-audio-plug-ins/22950)). Plan: start with continuous + vsync. Throttle to 30 fps when idle (no flights, no hover, energy below a threshold for 2 s) by switching to `triggerRepaint()` from a 30 Hz timer. Stop entirely when the editor is not showing (`isShowing()` false / minimised).

---

## 10. JUCE 8 specifics

Verified against `SDK/JUCE/modules/juce_opengl/opengl/juce_OpenGLContext.h` (8.0.8): `setRenderer`, `setPixelFormat`, `setMultisamplingEnabled`, `setOpenGLVersionRequired`, `setComponentPaintingEnabled`, `setContinuousRepainting`, `executeOnGLThread`, `getRenderingScale`, `isCoreProfile`, `setImageCacheSize`, `setNativeSharedContext`. The JUCE `develop` branch's OpenGLDemo now uses `setPreferredVersion({3,2})` / `setPreferredProfile(core)` ([JUCE OpenGLDemo.h](https://github.com/juce-framework/JUCE/blob/master/examples/GUI/OpenGLDemo.h)). On 8.0.8 use `setOpenGLVersionRequired(OpenGLContext::openGL3_2)`, which is a preference that some platforms may ignore ([JUCE 8 OpenGLContext docs](https://docs.juce.com/master/classOpenGLContext.html); [ccoderun JUCE 8.0.9 docs](https://ccoderun.ca/programming/doxygen/juce/classjuce_1_1OpenGLContext.html)). Check `isCoreProfile()` at runtime.

Setup (editor constructor, before `attachTo`):
```cpp
m_gl.setOpenGLVersionRequired(juce::OpenGLContext::openGL3_2);
juce::OpenGLPixelFormat pf; pf.depthBufferBits = 24; pf.stencilBufferBits = 8; pf.multisamplingLevel = 4;
m_gl.setPixelFormat(pf);
m_gl.setMultisamplingEnabled(true);
m_gl.setRenderer(&m_shipScene);          // ShipScene : juce::OpenGLRenderer
m_gl.setComponentPaintingEnabled(true);  // 2D JUCE components still drawn on top
m_gl.setContinuousRepainting(true);
m_gl.attachTo(*this);
// destructor: m_gl.detach();  (before m_shipScene is destroyed)
```

Renderer callbacks (all on the **GL thread**, not the message thread):
- `newOpenGLContextCreated()`: compile shaders (`juce::OpenGLShaderProgram::addVertexShader/addFragmentShader/link`), create VAO + VBOs, upload static meshes and textures. Using `OpenGLHelpers::translateVertexShaderToV3/translateFragmentShaderToV3` lets one source run on GL2 and GL3 like the demo does. Since we target core, it is simpler to write `#version 150` directly and fail gracefully (show the old 2D view) if `link()` fails, logging `getLastError()`.
- `renderOpenGL()`: `using namespace juce::gl;` (JUCE 7+ loads GL entry points into `juce::gl`, which the demo opens in each function). Set the viewport to `roundToInt(scale * getWidth())` using `getRenderingScale()` for Retina. Clear, draw sky, then ocean, then ship, then outlines, then particles (blend on, depth write off), then trails. **Unbind VAO/program and restore state** (depth test off, blend default, `glBindBuffer(0)`), because JUCE's component painting runs after `renderOpenGL` in the same context and the demo notes that the 2D renderer and your state interfere.
- `openGLContextClosing()`: delete all GL objects while the context is still current (the demo frees resources here). Hosts can close and recreate the context (editor hidden, moved offscreen, window docked), so creation must be idempotent.
- **Core profile + 3.2 requires a bound VAO** for any draw. Create one VAO per mesh.
- Data in: one `SceneInput` struct written by the message thread (UI params: aim, sails, flag, hover id, cargo list version) behind a `juce::SpinLock` that is held for a memcpy only, or a seqlock. Audio data is read straight from the engine's atomics.
- Data out: the GL thread publishes `{view, proj, viewport, object bounds}` for picking via the same mechanism.
- `juce::Graphics` overlays on top of the 3D: either (a) normal child components with `setComponentPaintingEnabled(true)`, which are easiest and already in use, or (b) inside `renderOpenGL` via `createOpenGLGraphicsContext(m_gl, w, h)` + `Graphics g(*ctx); g.addTransform(AffineTransform::scale(scale))`, as in the demo's `drawBackground2DStuff`. Use (a) for UI. Use (b) only for in-scene text that must be depth-sorted (we use the label texture atlas instead).
- Context sharing: one context per editor. Do not share across plugin instances. Each instance must stand alone (`setNativeSharedContext` is rarely needed and risky in hosts).

---

## 11. Plugin-host pitfalls (and mitigations)

1. **macOS OpenGL is deprecated (since 10.14) but still shipping, capped at 4.1.** Apple has never said when removal will happen ([AnandTech](https://home.anandtech.com/print/12894/apple-deprecates-opengl-across-all-oses); [Phoronix](https://www.phoronix.com/news/Apple-Deprecates-OpenGL-OpenCL)). On Apple Silicon it runs over Apple's Metal-backed GL driver. Mitigation: keep scene logic (`ShipModel`, wave math, particles, picking) **API-agnostic** and isolate GL in `GLBackend`. Then a future Metal / WebGPU / visage backend replaces one layer. Keep the 2D `SeaVisualiser` as a fallback if context creation or shader link fails.
2. **Multiple instances / multiple contexts on macOS** have historically caused intermittent rendering issues and message-manager lockups in some hosts (Ableton). Mixed JUCE versions in one process are also a factor ([Mojave multiple contexts](https://forum.juce.com/t/osx-mojave-openglcontext-attachto-issue-if-more-than-one-context-is-used/31055); [crash with multiple instances](https://forum.juce.com/t/crash-with-multiple-plugin-instances-using-opengl/11276)). Mitigation: never block the message thread waiting on the GL thread (`executeOnGLThread(..., false)` only). Test with 8 open editors in Live, Logic, Reaper and Pro Tools.
3. **Hosts moving editors offscreen or re-parenting** detach the context, and it may not come back ([Wavelab VST2](https://forum.juce.com/t/plugins-with-opengl-dont-work-as-vst2-under-wavelab-on-macos/19700)). Mitigation: idempotent `newOpenGLContextCreated`. Optionally re-`attachTo` in `parentHierarchyChanged` / `visibilityChanged` if `!isAttached()`.
4. **Detach order**: call `m_gl.detach()` in the editor destructor **before** members it renders are destroyed. Deleting `OpenGLTexture`s while the context is inactive asserts ([GLTexture release problem](https://forum.juce.com/t/gltexture-release-problem-on-windows-juce-develop/53279)), so free in `openGLContextClosing`. Cubase/Windows freezes have been tied to locks in the render path ([Cubase + OpenGL + locks](https://forum.juce.com/t/how-to-kill-cubase-with-juce-plug-in-s-opengl-some-locks-windows/25458)). No locks shared with the audio thread, and a minimal spin lock with the message thread.
5. **Windows CPU cost** of continuous repaint (see section 9). Also, Windows hosts can be per-monitor DPI aware or not. Always use `getRenderingScale()` and rebuild the FBO when it changes.
6. **Older macOS GL renderer failures in JUCE 7+** ([JUCE 7 OpenGL fails on older macOS](https://forum.juce.com/t/juce-7-opengl-renderer-fails-on-older-macos/52340)). Set a minimum macOS of 11 and keep the fallback.
7. **Legacy GL in existing code** (`LogoOutline` uses `glBegin`): this will produce `GL_INVALID_OPERATION` in core profile. Port it.
8. **Resizing**: the editor allows 0.6x-2x. Shaders take `uViewport` and outline width in pixels times scale. The label atlas is rendered at 2x for crispness.
9. **Do not render when hidden**: hosts keep editors alive in hidden windows. Check `isShowing()` and use `setContinuousRepainting(false)` when hidden.

---

## 12. Code-level plan

### 12.1 Files / classes (all new under `Source/Ship/`)
| Class | Thread | Responsibility |
|---|---|---|
| `ShipScene : juce::OpenGLRenderer` | GL | owns everything below. Implements the 3 callbacks. Falls back gracefully |
| `SceneInput` / `SceneOutput` | shared | POD snapshots (UI to GL / GL to UI picking matrices), seqlock |
| `AudioSnapshot` | GL | reads `FlightView[4]`, energy, peak, pulls FIFO samples, runs FFT, outputs smoothed `bands[8]`, `energy`, flight events |
| `WaveField` | GL + tests | the 8-wave set, `evaluate(xz, t)` in C++ (height, normal, J), maps audio to steepness. Has a unit test vs a reference sample |
| `ShipPose` | GL | 4-point Gerstner sampling, damping, heave/pitch/roll to model matrix |
| `MeshBuilder` | any (CPU) | lathe/loft/box/cylinder/quad, flat + smooth normals, transforms |
| `ShipBuilder` | message (at construction) | builds hull/masts/cannons/barrels/sails/flag into `MeshData` |
| `GLMesh` | GL | VAO + VBO (+ IBO) wrapper, `upload(MeshData)`, `draw()`, `drawInstanced()` |
| `ShaderLibrary` | GL | compiles the 7 programs below, caches uniform locations |
| `OceanPass`, `SkyPass`, `ToonPass` (+ outline), `ParticlePass`, `TrailPass` | GL | one draw routine each |
| `CannonFX` | GL | flight event to flash/recoil/smoke/ball/splash. Owns the particle pool and trails |
| `LabelAtlas` | message draws `juce::Image`, GL uploads | barrel labels + flag texture |
| `ScenePicker` | message | ray vs bounds, hover id, drag to parameter mapping (rubber band) |

The editor replaces `SeaVisualiser` with a transparent `ShipView` component that takes mouse input and file drops (`FileDragAndDropTarget`). It forwards drops to `Cargo` (which classifies and names), and forwards gestures to `ScenePicker`.

### 12.2 Vertex formats
```cpp
struct VtxLit     { float pos[3]; float nrm[3]; float smoothNrm[3]; uint8_t rgba[4]; float uv[2]; float objectId; }; // 48 B, ship/props/sails/flag
struct VtxOcean   { float xz[2]; };                                                                             //  8 B
struct VtxParticle{ float pos[3]; float size; float age; float kind; uint8_t rgba[4]; };                        // 28 B, per-instance, divisor 1
struct VtxTrail   { float pos[3]; float side; float t; };                                                       // 20 B, rebuilt per frame
```
`objectId` lets one shader do hover highlighting (`uHoverId`) and sail-specific deformation (`objectId` >= 100 means sail, with the sail index encoded).

### 12.3 Shader programs (GLSL 150)
1. `sky` (fullscreen triangle, gradient + moon + stars + logo lines)
2. `ocean` (Gerstner VS, toon foam FS)
3. `toon` (ship/props. VS handles sail billow and flag wave by `objectId`)
4. `outline` (same VS deformation + clip-space extrusion, flat colour)
5. `particle` (instanced billboards, toon-stepped disc / flash star / droplet by `kind`)
6. `trail` (ribbon, alpha fade)
7. `lines` (rigging, aim arc, logo; replaces fixed-function)

Ocean vertex shader skeleton:
```glsl
#version 150
in vec2 aXZ;
uniform mat4 uViewProj; uniform vec3 uCamPos; uniform float uTime;
uniform vec4 uWaves[8];            // dir.xy, steepness, wavelength
uniform vec4 uRings[8];            // x, z, t0, strength
out vec3 vWorld; out vec3 vNormal; out float vJ; out float vFog;
const float G = 9.8, PI = 3.14159265;
void main() {
    vec2 xz = aXZ + uCamPos.xz;                 // grid follows camera
    vec3 p = vec3(xz.x, 0.0, xz.y);
    vec3 T = vec3(1,0,0), B = vec3(0,0,1); float J = 1.0;
    for (int i = 0; i < 8; ++i) {
        vec2 D = uWaves[i].xy; float s = uWaves[i].z; float k = 2.0*PI/uWaves[i].w;
        float c = sqrt(G/k); float f = k*(dot(D, xz) - c*uTime); float a = s/k;
        float sf = sin(f), cf = cos(f);
        p += vec3(D.x*a*cf, a*sf, D.y*a*cf);
        T += vec3(-D.x*D.x*s*sf, D.x*s*cf, -D.x*D.y*s*sf);
        B += vec3(-D.x*D.y*s*sf, D.y*s*cf, -D.y*D.y*s*sf);
        J -= s*sf;
    }
    for (int r = 0; r < 8; ++r) {               // cannonball splash rings
        float age = uTime - uRings[r].z; if (age < 0.0 || age > 3.0) continue;
        float d = distance(xz, uRings[r].xy);
        p.y += uRings[r].w * exp(-age*1.5) * sin(8.0*(d - age*5.0)) * smoothstep(age*5.0+2.0, age*5.0, d);
    }
    vWorld = p; vNormal = normalize(cross(B, T)); vJ = J;
    float dist = length(p - uCamPos); vFog = 1.0 - exp(-pow(dist*0.006, 2.0));
    gl_Position = uViewProj * vec4(p, 1.0);
}
```

### 12.4 Frame order
```
read SceneInput, AudioSnapshot.update(), WaveField.update(), ShipPose.update(), CannonFX.update(dt)
glViewport(scaled); glClear(COLOR|DEPTH)
SkyPass (depth off)
OceanPass (depth on, write on)
ToonPass: ship, sails, flag, cannons (instanced), barrels, balls
Outline pass: same meshes, cull front
Lines: rigging, aim arc
ParticlePass: alpha smoke (depth test on, write off, sorted coarsely), then additive flash/sparks
TrailPass (additive)
restore state, publish SceneOutput
-> JUCE then paints 2D components on top
```

### 12.5 Milestones (each one is shippable)
1. Core-profile context + sky + Gerstner ocean driven by energy/bands. Port `LogoOutline`. Fallback path.
2. Procedural ship + toon + outlines + bobbing on the wave field.
3. Cannon FX bound to `FlightView` (flash, smoke, ball, trail, splash, fly-by).
4. Scene-as-controls: picking, cannon aim arc, sails, flag, dice. Remove the 2D flight pad.
5. Drag-drop on the ship becomes barrels with classifier labels (dynamic tracks).
6. Perf pass: idle throttling, quality scaler, Windows iGPU test, multi-instance host matrix.

---

## 13. three.js equivalent (browser prototype, about 1 day)

Use it to test the look and the interaction mapping with the user before writing C++. Use plain `ShaderMaterial` for the ocean (same GLSL as above, so the shader ports back 1:1). Use `MeshToonMaterial` with a 3-step `gradientMap` (a 3-pixel `DataTexture`, procedural) for the ship, and an inverted-hull mesh for outlines. That avoids `OutlineEffect`, which forum users report not playing well with `MeshToonMaterial` in some setups ([three.js forum](https://discourse.threejs.org/t/outline-effect-not-work-use-with-meshtoonmaterial/40961)).

```html
<script type="importmap">{"imports":{"three":"https://unpkg.com/three@0.170.0/build/three.module.js"}}</script>
<script type="module">
import * as THREE from 'three';
const r = new THREE.WebGLRenderer({antialias:true}); r.setPixelRatio(devicePixelRatio);
r.setSize(1280,800); document.body.appendChild(r.domElement);
const scene = new THREE.Scene(); scene.fog = new THREE.FogExp2(0xd9707f, 0.006);   // rose horizon
const cam = new THREE.PerspectiveCamera(38, 1280/800, 0.1, 800); cam.position.set(0,2.2,0); cam.lookAt(0,3,-28);
const PAL = {navy:0x14213d, teal:0x2a9d8f, rose:0xe76f81, amber:0xf4a261};

// toon ramp (procedural, 3 steps)
const ramp = new THREE.DataTexture(new Uint8Array([90,160,255]), 3, 1, THREE.RedFormat);
ramp.minFilter = ramp.magFilter = THREE.NearestFilter; ramp.needsUpdate = true;
const toon = c => new THREE.MeshToonMaterial({color:c, gradientMap:ramp, flatShading:true});
const outlineMat = new THREE.MeshBasicMaterial({color:PAL.navy, side:THREE.BackSide});
function withOutline(mesh, w=0.06){            // inverted hull
  const o = new THREE.Mesh(mesh.geometry, outlineMat);
  o.onBeforeRender = () => {}; o.scale.multiplyScalar(1+w); mesh.add(o); return mesh; }

// hull: loft cross-sections
function hullGeometry(L=10,B=3.8,D=2.2,stations=16,pts=8){
  const pos=[]; const sec=[];
  for(let i=0;i<=stations;i++){ const u=i/stations;
    const beam=B*Math.pow(Math.sin(Math.PI*(0.08+0.92*u)),0.6), depth=D*(1-0.35*u**3);
    const sheer=0.4+0.8*(2*u-1)**2;
    const ring=[]; for(let j=0;j<=pts;j++){ const t=j/pts;
      ring.push(new THREE.Vector3(beam*Math.pow(t,0.45), -depth*(1-t)+sheer*t, (u-0.5)*L)); }
    sec.push(ring); }
  for(let i=0;i<stations;i++) for(let j=0;j<pts;j++) for(const side of [1,-1]){
    const a=sec[i][j],b=sec[i+1][j],c=sec[i+1][j+1],d=sec[i][j+1];
    const q=[a,b,c,a,c,d].map(v=>[v.x*side,v.y,v.z]); if(side<0) q.reverse(); pos.push(...q.flat()); }
  const g=new THREE.BufferGeometry(); g.setAttribute('position',new THREE.Float32BufferAttribute(pos,3));
  g.computeVertexNormals(); return g; }
const ship = new THREE.Group(); ship.position.set(0,0,-28); scene.add(ship);
ship.add(withOutline(new THREE.Mesh(hullGeometry(), toon(0x8b4a3c))));
for (const z of [-2,1.5]) { const m=new THREE.Mesh(new THREE.CylinderGeometry(0.12,0.18,9,6), toon(0x5a3a2a)); m.position.set(0,5,z); ship.add(withOutline(m)); }

// sail with billow in onBeforeCompile
const sailGeo = new THREE.PlaneGeometry(4.5,4,12,10);
const sailMat = toon(0xf3ead8); const sailU = {uWind:{value:0.5}, uTime:{value:0}};
sailMat.onBeforeCompile = s => { Object.assign(s.uniforms, sailU);
  s.vertexShader = 'uniform float uWind,uTime;\n' + s.vertexShader.replace('#include <begin_vertex>',`
    vec3 transformed = position; float su=uv.x, sv=1.0-uv.y;
    float belly = sin(3.14159*su)*pow(sin(3.14159*clamp(sv*0.9+0.1,0.,1.)),0.8);
    transformed.z += belly*(uWind*1.2 + 0.08*sin(uTime*3.1+su*5.+sv*2.)*uWind);`); };
const sail = new THREE.Mesh(sailGeo, sailMat); sail.position.set(0,5.5,-2.3); ship.add(sail);

// ocean: Gerstner ShaderMaterial (same GLSL as the C++ plan, minus #version)
const waves = Array.from({length:8},(_,i)=>new THREE.Vector4(Math.cos(i*0.7),Math.sin(i*0.7),0.08,[50,40,18,12,8,4,3,2][i]));
const oceanU = {uTime:{value:0}, uWaves:{value:waves}, uCam:{value:cam.position},
  uDeep:{value:new THREE.Color(PAL.navy)}, uMid:{value:new THREE.Color(PAL.teal)}, uFog:{value:new THREE.Color(0xd9707f)}};
const ocean = new THREE.Mesh(new THREE.PlaneGeometry(600,600,256,256).rotateX(-Math.PI/2),
  new THREE.ShaderMaterial({uniforms:oceanU, vertexShader:/*glsl*/`
    uniform float uTime; uniform vec4 uWaves[8]; varying vec3 vN; varying float vJ; varying vec3 vW;
    void main(){ vec3 p=position; vec2 xz=p.xz; vec3 T=vec3(1,0,0),B=vec3(0,0,1); float J=1.;
      for(int i=0;i<8;i++){ vec2 D=normalize(uWaves[i].xy); float s=uWaves[i].z,k=6.2832/uWaves[i].w,c=sqrt(9.8/k);
        float f=k*(dot(D,xz)-c*uTime),a=s/k; p+=vec3(D.x*a*cos(f),a*sin(f),D.y*a*cos(f));
        T+=vec3(-D.x*D.x*s*sin(f),D.x*s*cos(f),-D.x*D.y*s*sin(f)); B+=vec3(-D.x*D.y*s*sin(f),D.y*s*cos(f),-D.y*D.y*s*sin(f)); J-=s*sin(f);}
      vN=normalize(cross(B,T)); vJ=J; vW=p; gl_Position=projectionMatrix*viewMatrix*vec4(p,1.);}`,
  fragmentShader:/*glsl*/`
    uniform vec3 uDeep,uMid,uFog,uCam; varying vec3 vN; varying float vJ; varying vec3 vW;
    void main(){ vec3 L=normalize(vec3(-0.4,0.5,-1.)); float b=step(0.2,dot(vN,L));
      vec3 col=mix(uDeep,uMid,0.35+0.4*b); col=mix(col,vec3(1.),step(vJ,0.55));         // crest foam
      float spec=step(0.97,dot(reflect(normalize(vW-uCam),vN),L)); col+=spec*vec3(1.,.9,.7);
      float d=length(vW-uCam); col=mix(col,uFog,1.-exp(-pow(d*0.006,2.)));
      gl_FragColor=vec4(col,1.);}`}));
scene.add(ocean);
scene.add(new THREE.DirectionalLight(0xbfd6ff, 2.2).translateX(-4).translateY(6).translateZ(-10));
scene.add(new THREE.HemisphereLight(0x2a3d66, 0x14213d, 0.8));
scene.background = new THREE.Color(PAL.navy);          // swap for a gradient sky ShaderMaterial on a big sphere

// fake "audio": energy from mic or a slider, flights on click
let energy = 0.3; addEventListener('pointerdown', fire);
const balls = [];
function fire(){ const b = new THREE.Mesh(new THREE.IcosahedronGeometry(0.25,1), toon(0x222222));
  b.userData.t0 = performance.now()/1000; scene.add(withOutline(b)); balls.push(b); energy = Math.min(1, energy+0.3); }
function flightPos(age, x){  // same mapping as section 7
  const muzzle=new THREE.Vector3(0,2.5,-23), impact=new THREE.Vector3(6,0,4);
  return muzzle.lerp(impact,age).add(new THREE.Vector3(x*3,4*age*(1-age)*5,0)); }

// ship bob: evaluate the same waves on the CPU
function waveHeight(x,z,t){ let y=0; for(const w of waves){ const k=6.2832/w.w,c=Math.sqrt(9.8/k);
  const D=new THREE.Vector2(w.x,w.y).normalize(); y+=(w.z/k)*Math.sin(k*(D.x*x+D.y*z-c*t)); } return y; }

r.setAnimationLoop(ms=>{ const t=ms/1000; oceanU.uTime.value=t; sailU.uTime.value=t;
  energy += (0.25-energy)*0.01; sailU.uWind.value=energy;
  waves.forEach((w,i)=> w.z = (i<2?0.10+0.2*energy:0.04+0.06*energy));       // the sea follows the sound
  const bow=waveHeight(0,-33,t), stern=waveHeight(0,-23,t), port=waveHeight(-2,-28,t), stbd=waveHeight(2,-28,t);
  ship.position.y=(bow+stern+port+stbd)/4; ship.rotation.x=Math.atan2(bow-stern,10); ship.rotation.z=Math.atan2(port-stbd,4);
  for(const b of balls){ const age=(t-b.userData.t0)/1.6; b.position.copy(flightPos(Math.min(age,1),Math.sin(age*3)*0.2)); if(age>1) b.visible=false; }
  r.render(scene,cam); });
</script>
```
Prototype extras worth adding (each about 20 lines): a gradient sky sphere with a moon, `Points`-based smoke with a toon-stepped sprite drawn into a `CanvasTexture`, and a `Raycaster` for picking the cannon/sails, to test the drag mappings. To feed real audio, use `AnalyserNode.getByteFrequencyData` folded to 8 bands, which is the same mapping as `AudioSnapshot`.

---

## 14. Open questions for Samuel
1. Camera as listener (balls fly past *you*) vs. a side-on "theatre" view (balls fly across). The listener view sells doppler best. The side view makes barrels/sails easier to grab. Recommendation: listener view, ship at 3/4 angle.
2. Should every whoosh be a cannon? Alternative: "flavour by cargo". Fire samples shoot flaming balls, cloth samples shoot chain-shot that spins, metal shoots grapeshot. This is fun and reuses classifier labels, and is cheap to add once `CannonFX` exists.
3. Is the "More" drawer (moon click) acceptable as the only route to mastering/tape, or does tape need a visible "ship's log" button on screen at all times?

## Sources
- JUCE OpenGLDemo (shader compile, VBO/attributes, `juce::gl`, lifecycle, 2D overlay, rendering scale): https://github.com/juce-framework/JUCE/blob/master/examples/GUI/OpenGLDemo.h
- JUCE OpenGLContext docs: https://docs.juce.com/master/classOpenGLContext.html ; https://ccoderun.ca/programming/doxygen/juce/classjuce_1_1OpenGLContext.html
- JUCE forum: continuous repaint CPU: https://forum.juce.com/t/br-fr-opengl-context-with-continuous-repaint-hogs-one-core-100/61253 ; https://forum.juce.com/t/br-opengl-is-using-a-lot-of-cpu/50288 ; https://forum.juce.com/t/low-fps-with-opengl-rendering-in-audio-plug-ins/22950
- JUCE forum: host pitfalls: https://forum.juce.com/t/osx-mojave-openglcontext-attachto-issue-if-more-than-one-context-is-used/31055 ; https://forum.juce.com/t/crash-with-multiple-plugin-instances-using-opengl/11276 ; https://forum.juce.com/t/plugins-with-opengl-dont-work-as-vst2-under-wavelab-on-macos/19700 ; https://forum.juce.com/t/how-to-kill-cubase-with-juce-plug-in-s-opengl-some-locks-windows/25458 ; https://forum.juce.com/t/gltexture-release-problem-on-windows-juce-develop/53279 ; https://forum.juce.com/t/juce-7-opengl-renderer-fails-on-older-macos/52340 ; https://forum.juce.com/t/is-opnegl-3-2-supported-on-windows/14822
- Apple OpenGL deprecation: https://home.anandtech.com/print/12894/apple-deprecates-opengl-across-all-oses ; https://www.phoronix.com/news/Apple-Deprecates-OpenGL-OpenCL
- Gerstner waves: https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models ; https://catlikecoding.com/unity/tutorials/flow/waves/ ; https://80.lv/articles/ocean-shader-with-gerstner-waves-in-unity
- Toon water / Wind Waker: https://medium.com/@gordonnl/wind-waker-graphics-analysis-a0b575a31127 ; https://godotshaders.com/shader/wind-waker-water-no-textures-needed/ ; https://godotshaders.com/shader/toon-style-3d-water-shader-no-textures-needed/ ; https://github.com/IronWarrior/ToonWaterShader ; https://discourse.threejs.org/t/toon-water-shader-with-depth-based-fog-and-intersection-foam/35978 ; https://alexanderameye.github.io/notes/stylized-water-shader ; https://discussions.unity.com/t/how-to-color-a-complex-ocean-waves-shader-with-vertex-displacement/1547370
- Toon shading and outlines: https://click.gamedev.net/tutorials/programming/graphics/toon-shader-using-unity-r5095 ; https://bnpr.gitbook.io/bnpr/outline/inverse-hull-method ; https://github.com/Delt06/urp-toon-shader/wiki/Outline ; https://forums.unrealengine.com/t/best-way-to-fix-gaps-in-toon-outlines-when-using-inverted-hull-method/1319625 ; https://en.wikipedia.org/wiki/Cel_shading
- Sails/flags via vertex shader: https://www.moddb.com/news/vertex-shader-fun-august-15-2019 ; https://lukasstratmann.artstation.com/projects/LeR1k5
- three.js: https://github.com/mrdoob/three.js/blob/dev/examples/webgl_shaders_ocean.html ; https://discourse.threejs.org/t/when-copying-the-code-for-water-ocean-object-examples-the-water-is-rendered-black/24935 ; https://discourse.threejs.org/t/outline-effect-not-work-use-with-meshtoonmaterial/40961
