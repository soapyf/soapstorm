# Atmo Magic: wet surfaces and rain drops on alpha-blended geometry (glass)

Design for reaching the one class of surface the surface-weather feature structurally cannot touch:
**alpha-BLENDED** geometry - the transparency slider, `DIFFUSE_ALPHA_MODE_BLEND` materials, GLTF
`ALPHA_MODE_BLEND`. Written 2026-09-07 on `feature-atmo-magic`, against the design in
`doc/atmo_magic_surface_weather.md` and the verified architecture map in `doc/viewer/frame_pipeline.md`
and `doc/viewer/glow_and_alpha.md`. Prompted by the user's verdict on the phase-2 build: the drops are
"not seeing them on the transparent glass surfaces, which is what I was looking most forward to".

Nothing here changes the three G-buffer passes, the field, the cell state or the post chain. It adds a
fourth evaluation site for the SAME laws, inside the forward alpha material shaders.

## 1. The diagnosis

The wet, albedo and normal passes are **screen-space passes over the G-buffer**. `ssSurfaceWetF.glsl`
says so in its own header ("A screen space pass over the gbuffer"); it samples `diffuseRect` and
`specularRect`, reconstructs agent space through `ssFieldInvView`, and writes G-buffer attachments 1
and 2 through a raw `glDrawBuffers` (`sssurfacefield.cpp:1552`, restored at `:2050`). All three are
drawn from inside `renderDeferredLighting`: `renderAlbedoPass()` at `pipeline.cpp:9967`,
`renderWetPass()` at `pipeline.cpp:9969`, both **before** `screen_target->bindTarget()` at `:9971`.
The placement comment at `:9968` states the reason and it is the right reason: "Ahead of every lighting
pass below, so the sun, the local lights, the projectors and the probes all read one consistent gbuffer
rather than each being taught about the weather on its own."

That placement buys consistency and it costs exactly one thing: **anything that does not write the
G-buffer is invisible to the weather**. Alpha-blended geometry is drawn much later, in the forward stage,
by `LLDrawPoolAlpha` from `LLPipeline::renderGeomPostDeferred` (`pipeline.cpp:4408`), into the already-lit
`mRT->screen` target. Its pools are `POOL_ALPHA_PRE_WATER` and `POOL_ALPHA_POST_WATER`
(`pipeline.cpp:5974`, `:5985`). It never touches `deferredScreen`. The wet pass ran four thousand lines
of frame earlier, over a G-buffer in which the glass does not exist, and it cannot reach it. This is not
a bug in the wet pass; it is the deferred contract.

**Alpha-MASKED geometry is a different animal and it works.** A masked (cutout) face goes to
`PASS_ALPHA_MASK`, whose pool is `LLDrawPoolAlphaMask` - and that pool is a **deferred** pool:
`getNumDeferredPasses() { return 1; }` (`lldrawpoolsimple.h:84`) with `renderDeferred` binding
`gDeferredDiffuseAlphaMaskProgram` and pushing mask batches (`lldrawpoolsimple.cpp:116-128`). Masked
faces discard, they do not blend, and every surviving fragment writes the full G-buffer exactly like an
opaque one. The same split is visible inside `materialF.glsl` itself: `DIFFUSE_ALPHA_MODE_BLEND` is 1
(`class1/deferred/materialF.glsl:30`), and the file's whole output path forks on it -
`#if (DIFFUSE_ALPHA_MODE == DIFFUSE_ALPHA_MODE_BLEND)` writes a single forward `frag_color` (`:32`,
`:40`), `#else` "encode to gbuffer" writes `frag_data[4]` (`:42`). One file, two universes.

**Confirmed in the live build**, not inferred: the user reports "Yeah I am seeing it on masked
surfaces." So the diagnosis is narrower and better than "the feature does not reach transparent
geometry". It is:

> Everything the surface-weather feature does - wetness, darkening, roughness tightening, puddles, the
> drop lattice, the drip lattice, sheet flow, deposits, ice, frost - already works, on the G-buffer, on
> every opaque and every alpha-MASKED surface in the world. The single missing case is alpha-BLENDED
> geometry, and it is missing for one structural reason: those fragments never write the G-buffer.

That matters for scoping. Whatever we build here does not have to re-implement the feature, re-derive
the laws or re-litigate the looks. **The drop law is proven in the field on real geometry.** It has to
carry an already-working, already-tuned law across one pipeline boundary. The risk is not correctness;
the risk is drift between the two sites, which is precisely what the ONE FORMULA SITE rule
(`doc/atmo_magic_architecture.md:100`) exists to prevent.

### What the user should be seeing today, and where the boundary sits

Wet, drops and drips today: every opaque prim and mesh, terrain, every alpha-masked face (leaves,
chain-link, cutout signage, most trees), avatars via their own `ssavatarwet` model. Not: any face whose
texture-entry colour alpha is under 0.999 (`llvovolume.cpp:6967-6970`, `blinn_phong_transparent` ORs into
`is_alpha`), any Blinn-Phong material set to `DIFFUSE_ALPHA_MODE_BLEND`, any GLTF material with
`ALPHA_MODE_BLEND` (`llvovolume.cpp:6979-6982`), and - a second gap worth naming while we are here -
**anything fullbright**, because `LLDrawPoolFullbright` and `LLDrawPoolFullbrightAlphaMask` are
post-deferred pools too (`lldrawpoolsimple.h:101`, `:119`). A fullbright pane is a forward surface even
when it is fully opaque. Section 9, question 1 says what to do about that.

The user dragged a prim's transparency slider to make a window. That is the first line of
`llvovolume.cpp:6967`. It is the whole bug.

## 2. The options

### The constraint that eliminates a whole family of answers first

**Alpha-blended geometry writes no depth.** `LLDrawPoolAlpha::forwardRender` computes
`write_depth = rigged || sSkipScreenCopy || sImpostorRenderAlphaDepthPass || type == POOL_ALPHA_PRE_WATER`
(`lldrawpoolalpha.cpp:242-246`) and then `LLGLDepthTest depth(GL_TRUE, write_depth ? GL_TRUE : GL_FALSE)`
(`:249`). For the ordinary case - a static glass pane above the water line, in `POOL_ALPHA_POST_WATER` -
every one of those is false and depth writes are **off**. There is a late sub-pass that writes depth for
faces over 33% opaque (`:212-228`, `setMinimumAlpha(0.33f)`, colormask off) but it only runs when
`RenderDepthOfField` is on, only for `POOL_ALPHA_POST_WATER`, not in cube snapshots or impostors, and it
runs *after* the colour is already composited.

Therefore **no screen-space pass can find the glass**: there is no depth to reconstruct a position
from, no normal in the G-buffer, and the albedo behind the pane belongs to the wall behind it. Every
"just add another fullscreen pass" answer - a deferred decal over alpha depth, a second wet pass after
the alpha pool, an OIT or depth-peel layer - dies on this line. Recorded so nobody proposes it again.

### (a) Evaluate the layer inside the forward alpha material shaders

The honest count. Alpha-blended world surfaces are drawn by these **fragment shader files**, with these
**linked programs** behind them:

| File | Programs bound to it | Count |
|---|---|---|
| `class2/deferred/alphaF.glsl` | `gDeferredAlphaProgram`, `gDeferredSkinnedAlphaProgram`, `gDeferredAlphaImpostorProgram`, `gDeferredSkinnedAlphaImpostorProgram`, `gHUDAlphaProgram`, `gDeferredAvatarAlphaProgram` | 6 |
| `class1/deferred/materialF.glsl` + `class3/deferred/materialF.glsl` | `gDeferredMaterialProgram[i]` where `(i & 0x3) == 1` (`llviewershadermgr.cpp:1459`, `:1482`) - i.e. i = 1, 5, 9, 13, 17, 21, 25, 29 out of the 32 (`LLMaterial::SHADER_COUNT` 16, doubled for rigged) | 8 |
| `class1/deferred/pbralphaF.glsl` + `class2/deferred/pbralphaF.glsl` | `gDeferredPBRAlphaProgram`, `gDeferredSkinnedPBRAlphaProgram`, `gHUDPBRAlphaProgram` | 3 |
| `class1/deferred/fullbrightF.glsl` | `gDeferredFullbrightAlphaMaskAlphaProgram`, `gHUDFullbrightAlphaMaskAlphaProgram` (blended), sharing the file with 4 masked/opaque siblings | 2 |

**Six fragment shader files, nineteen linked programs**, of which three are HUD-only. That is the number
the info-view Look decision was weighing when it rejected the material-path route
(`doc/atmo_magic_phase8_show.md` section 6 item 4: "this fork has dozens of material paths, and every
alpha-blended surface would need its own variant to composite correctly").

**This case is not that case, and the difference is the whole argument.** The Look wanted to *replace*
each path's lighting result with a flat warm-gray - a different rewrite per path, because each path's
lighting math is different, with no shared body to hoist. The drop layer wants to *modify* one input
(the shading normal) and *add* one output term (a specular lobe and a small opacity lift), after each
path has computed its own lighting the way it already does. The body is identical in all six files. So
(a) here is six small call sites into **one** shared include, not six divergent rewrites, and the
constants live in `sssurfacestatecore.h` where they already are. The Look's rejection stands on its own
facts and this decision agrees with its reasoning while landing on the other branch, because the shape of
the change is different.

**The strong argument for (a), and it is strong.** A forward material shader has the surface's own
geometry natively:

- `vary_position` is the fragment's **view-space** position, interpolated from the vertex shader's
  `modelview_matrix * vert` (`class1/deferred/alphaV.glsl:106`, `:119`; `class2/deferred/alphaF.glsl:46`;
  `class2/deferred/pbralphaF.glsl:56`; `class3/deferred/materialF.glsl:52`). It is an interpolated float
  attribute of a small camera-relative magnitude - not a value recovered from a nonlinear depth buffer
  through `inv_proj`. One `ssFieldInvView` multiply turns it into agent space and it is done.
- `vary_norm` / `vary_normal` is the true shading normal (`alphaF.glsl:48`, `pbralphaF.glsl:65`,
  `materialF.glsl:211/216`), with the tangent frame beside it where a normal map exists - not a normal
  re-encoded into 10 bits per channel and read back out of attachment 2.
- `gl_FrontFacing` is available, which no screen-space pass has and which glass specifically needs
  (section 4).

So the glass path needs **no depth reconstruction, no `inv_proj`, no `screen_res`, and no F32 precision
loss at distance.** It cannot acquire the class of anchoring defect the opaque path is being debugged
for right now (the user: surface drops "seem unstable and move erratically when I move the camera").
That is not a convenience; it is a structural immunity, and it is the single biggest reason to choose
this route.

The honest cost of (a): six files whose stock text this fork now carries a variant of (behind
`SS_ATMO`, `doc/viewer/soapstorm_layer.md`); every file must re-declare every uniform and prototype
every function it calls, because `LLShaderMgr::loadShaderFile` compiles each `mShaderFiles` entry as its
own translation unit and GLSL link merges matching uniform declarations but propagates nothing else
(`doc/atmo_magic_surface_weather.md` section 3 - the rule that already bit this feature once, as AUDIT
finding 10 at `sssurfacefield.cpp:2104`, when the albedo pass read `ssWetFlattenCosZero/Full` as GL's
default zero because nothing bound them for *that* program). Every program that links the include needs
its own per-frame uniform upload, because uniform locations resolve per program.

### (b) A dedicated forward pass that re-draws wettable transparent surfaces

Draw the alpha pool's batches a second time, after the pool, with a drop-only shader blended on top.

Cost, stated honestly. Sorting: the alpha pool sorts and draws by spatial group, interleaving with
water and particles; a second pass after the whole pool composites **all** drops on top of **all**
alpha, so a drop on a far pane draws over a near pane in front of it. Two panes of glass at an angle -
which is what a greenhouse, a shopfront or a car is - render wrong. Draws: one extra `drawRange` per
wettable alpha batch, plus a full second rasterisation of every one of those faces, which is the most
overdraw-sensitive part of an SL frame. Vertex work: the second pass must reproduce each host's vertex
transform **exactly** or the re-draw z-fights and cracks - and "each host" means `alphaV`, `materialV`,
`pbralphaV` and `fullbrightV`, each with a rigged variant needing the matrix palette
(`uploadMatrixPalette`, `lldrawpoolalpha.cpp:549`). So (b)'s advertised "one new shader pair" is
actually four vertex paths plus their rigged twins, and it still gets the sort order wrong. **Rejected.**

### (c) A sub-pass inside `LLDrawPoolAlpha`'s own group loop

The refinement of (b) that fixes its sorting: the pool already re-draws the same batches with a
different shader and a different blend, per spatial group, inside its own iteration - that is
`renderEmissives` / `renderPbrEmissives` / `renderRiggedEmissives`
(`lldrawpoolalpha.cpp:509-580`), collected into per-group vectors at `:831-846` and flushed at the end
of that group at `:871-896`. A `renderWetDrops` twin of that shape would be one C++ site and would ride
the pool's existing group-granular sort.

It still pays (b)'s two real costs: the second rasterisation of every wettable alpha face, and the four
vertex paths plus rigged variants. And it buys nothing (a) does not already have, because the host
shader is right there with the geometry in hand. **Rejected**, but recorded as the correct shape if
(a) ever proves impossible - it is a materially better version of (b) and the emissive sub-pass is the
working precedent for it.

### (d) Make blended geometry write the G-buffer

A "glass prepass" that writes depth+normal+albedo for the frontmost blended layer so the existing
screen-space passes find it. Rejected: it can only ever capture one layer, it would have the wet pass
darken and roughen whatever is *behind* the glass wherever the prepass won the depth test, it breaks the
soften pass's assumption that every G-buffer fragment is opaque, and it needs a fifth attachment or a
second full-size RT. The deferred contract is not negotiable for a decoration.

### The decision

**(a), scoped.** Do not touch all nineteen programs. Ship the three in-world blended paths that actually
carry glass:

| File | In-world blended programs it serves |
|---|---|
| `class2/deferred/alphaF.glsl` | `gDeferredAlphaProgram`, `gDeferredSkinnedAlphaProgram` |
| `class1/deferred/materialF.glsl`, `class3/deferred/materialF.glsl` | the 8 `gDeferredMaterialProgram[(i & 3) == 1]` |
| `class1/deferred/pbralphaF.glsl`, `class2/deferred/pbralphaF.glsl` | `gDeferredPBRAlphaProgram`, `gDeferredSkinnedPBRAlphaProgram` |

**Five shader files, twelve linked programs.** Excluded on purpose: the three HUD programs (a HUD is
not in the world and has no field position); the two impostor programs (an impostor is a cached snapshot
- drops baked into it would swim as the avatar turns, which is the exact defect this route exists to
avoid); `gDeferredAvatarAlphaProgram`, which shares `alphaF.glsl` but whose wetness belongs to
`ssavatarwet` and must not be fought over by two models
(`ssSurfaceFieldF.glsl:112` already hands avatars "no answer" for the same reason).

The gate for the excluded programs is free and needs no `#define`: the shell simply does not bind the
field for them. Every uniform then reads GL's default zero, `ssSurfaceDrops` is 0, and the layer is a
no-op. That is also the Atmo-off path, and rung R7 in section 6 pins it as bit-identical.

**The law sits in a core; the shaders transliterate it; a twin holds them together.** The drop, drip and
sheet laws are already in `sssurfacestatecore.h` and already transliterated into `ssSurfaceStateF.glsl`
(`ssDropLaw` at `:192`, `ssSheetLaw` at `:203`, the `SS_DROP_*` / `SS_DRIP_*` / `SS_SHEET_*` constants at
`:63-72`). The new work goes into **one** new shared shader file, `ssSurfaceGlassF.glsl`, added to each
of the twelve programs' `mShaderFiles`; the five host files gain a prototype block and a single call
site each. Because the body is written once, **one twin** is needed:
`V:\Scratch\surface\tests\twin_glass_glsl.cpp`, transliterating `ssSurfaceGlassF.glsl` against
`sssurfacestatecore.h` exactly as `twin_surface_glsl.cpp` already does for the three G-buffer passes.
One twin, not six - and that is only true because the body is hoisted. If a future change ever inlines
the layer into a host file "just for that one path", the count becomes six twins and the rule has been
broken; say so in the review.

## 3. What glass needs that opaque surfaces do not

**Both sides.** A drop on a window is seen from inside and out. A screen-space pass has no concept of
which side of a surface it is looking at; a forward shader has `gl_FrontFacing`. See section 4 - the
decision is that the pattern is built in the pane's own world frame from the *weather-facing* normal
regardless of the viewer, so it is identical from both sides, and section 6's rung R3 is what proves it.

**Refraction and reflection: glass is the middle case, and it gets both.** See section 3b, which is the
decision and the reason.

**Gravity down a vertical pane.** The release / run / stutter / trail model already exists on the surface
side at `ssSurfaceNormalF.glsl:279-330`: a lattice in the (horizontal tangent, world Z) frame with
`drip_t = normalize(cross(vec3(0,0,1), n_geo_world))` (`:167`), cells 0.06 m wide by 0.30 m tall
(`:169`), a per-cell release gate `dhash < drip_gate` (`:288`), a slide speed
`law.w * (0.15 + 0.35 * dhash)` (`:291`), a stutter `-0.05 * sin(ssTime*6.0 + dphase*TAU) * dhash`
(`:296`), a cap normal for the drop and a narrow vertical ridge for the trail (`:310`, `:323`), and a
wind skew of the lattice through `ssSurfaceWind` (`:168`). Glass takes that law verbatim. The next
section says which lens behaviours it does *not* take, and why the two systems are deliberately built
differently.

### 3a. Glass is stateless; the lens is stateful. This asymmetry is on purpose

The reference for drop behaviour is the Tympanus RainEffect demo, which is a **stateful simulation**: an
accumulating population of drops, release keyed on size, running heads that consume the fine droplets in
their path and grow as they do, medium beads pinched off into the wake. The user's own verdict on it is
that "this works best for the lens drops", and separately, on glass: **"the surface drops work best for
stateless esp as we move around the world we would be constantly starting with a fresh surface."**

That second verdict is settled and it is the reasoning this spec is built on, because it is a
**correctness** argument rather than a performance one - and the correctness argument is the stronger of
the two. A world surface streams into view *already wet*. It has been raining on that pane for ten
minutes before you rounded the corner. A stateful per-pane simulation has exactly two possible
behaviours the moment the pane enters the frustum, and both are wrong: pop in empty and visibly fill up
while you stand there watching it, or fabricate a warm start, which is an arbitrary invented history that
will not match the pane beside it or the same pane after you look away and back. A stateless law keyed on
the pane's own **world position** has the correct pattern already there the instant the pane appears,
identically every time, for every viewer - and it is correct for free, with nothing to seed, cull, evict
or clear across culling, occlusion, region crossings and teleports. (The cost of per-pane state was also
weighed - state that must survive culling, for an unbounded and constantly changing set of visible panes
- and it is not affordable either; but it is rejected on correctness first, and that is the reason to
record.)

**The architecture that falls out: split the stateful scalar from the stateless pattern.**

- **How wet a surface is** legitimately evolves over time and is legitimately remembered - and it already
  is, by the world-anchored surface field. Wetting up when the rain starts, holding, and drying out
  afterwards is `SSSurfaceField`'s settle pass and `mWet`, existing shipped machinery this spec does not
  have to invent and must not duplicate.
- **What the drops look like at that wetness** is a stateless procedural function of (world position,
  wetness, intensity, time). No per-drop state, no per-pane state, nothing kept between frames.

That division also disposes of accumulation cleanly: **accumulation is a property of the wetness scalar,
not of a drop list.** Glass gets accumulation - a pane that has been rained on for a minute is wetter and
carries more and larger runners than one that just got its first drops - without any simulation at all,
because `ssDropLaw(intensity)` and the field's `wet` between them already scale density, size, drip
density and drip speed. The look the RainEffect demo gets from a growing population, the surface path
gets from a rising scalar driving a procedural density.

Behaviour by behaviour:

| RainEffect behaviour | Lens | Glass | Why |
|---|---|---|---|
| Accumulating drop population | Yes, stateful | **No** - carried by the field's wetness scalar instead | A pane must arrive already wet; a lens never arrives, the viewer is always behind it |
| Release by size / gravity threshold | Yes, per drop | **Yes, procedurally** - the per-cell `dhash < drip_gate` release gate, cell-hashed rather than simulated | Same phenomenon, no per-drop state needed to express it |
| Running heads that consume droplets in their path and grow | Yes | **No** | Consumption is inherently a stateful interaction between two tracked drops; there is no stateless expression of it. Stated gap, and the honest reason the glass runners will read slightly more regular than the lens ones |
| Beads pinched off into the wake | Yes | **Approximated** - the trail ridge behind the sliding drop (`ssSurfaceNormalF.glsl:320-324`) | The visual result at a distance is the same; the mechanism is not |
| Gravity run-down on a vertical surface | Yes, screen-space `slideDir` | **Yes, world-space** - flow-aligned along the pane's own down-slope direction | Physically the same phenomenon and the one the user will most notice missing on a window |
| Condensation the runners cut clear | Yes | Follow-up (section 9, question 3) | Same law, different surface |

**Run-down streaks without per-drop state.** This is the one behaviour where the stateless constraint has
to earn its keep, and it does: the existing drip block is already a world-anchored, flow-aligned,
advected pattern. The lattice `(dot(p_world, drip_t), p_world.z)` is fixed in the world; the drop's
position within its cell advances with `ssTime` at a per-cell hashed speed; the trail is a function of the
distance behind that position. Nothing is remembered between frames, and yet a runner has a continuous
track, a stutter, and a fading wake. Scaling the streak length and the down-slope advection rate by the
field's `wet` gives the accumulation read directly: a barely-damp pane shows short, sparse, slow runners,
a soaked one shows long fast streaks joining into sheet flow through `ssSheetLaw`. **This is the
recommended construction and it needs no new law** - it is the surface drip block, applied on the blended
path, with `wet` sourced from `ssFieldAt` exactly as the opaque path sources it. Recorded so nobody
concludes the stateless choice cost us the run-down: it did not.

**What is and is not shared with the lens, precisely.** The *demand* and *state* laws are not shared and
must not be: the lens accumulates by camera pitch against the rain vector (`ssscreenfxcore.h`:
`LENS_UP_LO 0.05` / `LENS_UP_HI 0.70` / `LENS_DOWN_CUT -0.35` / `LENS_AHEAD_GAIN 0.5`, `lensDemand:152`)
and relaxes through `stepLens:169` with `LENS_WET_IN_S 1.5` / `LENS_WET_OUT_S 6`, because a lens is a
camera with a real, continuous, never-left history. A pane's wetness comes from the field. The *frame* is
not shared: the lens lattice is in screen units with a `slideDir` (`:196`) that is gravity's screen
projection plus `LENS_WIND_SLIDE 0.15` times the wind's, which is exactly the kind of camera-relative
anchoring a world surface must never use. What could reasonably be shared is the 1-D run kernel - given a
cell hash, a time and a speed, where is the head and how strong is the wake - which is spelled once in
`ssSurfaceNormalF.glsl` and again in `ssPostLensF.glsl`, in neither case in a core. **Recommendation:
hoist that kernel** into `sssurfacestatecore.h` as `SSSurfaceState::dripRun`, transliterated by the
surface pass, the glass include and the lens pass alike, pinned by a transliteration twin of the OLD text
per lesson 22 (`doc/atmo_magic_architecture.md:100`). Caveat: `ssPostLensF.glsl` is under active edit as
of 2026-09-07 and its stateful model is being built there, so the hoist is a later commit and may end up
covering only the kernel's shape rather than its whole body. Glass uses the surface drip law verbatim in
the meantime.

The asymmetry between the two systems is therefore deliberate and should never be read as an
inconsistency to be tidied up: **one surface that is always in view and never left gets a simulation; an
unbounded set of world surfaces that stream in already wet get a world-anchored procedural law.**

### 3b. Refraction, reflection and Fresnel: three systems, three shading models, one drop law

The user's rule, verbatim: **"surface drops should be entirely reliant on reflection probes I think, its
only lens drops that can make use of refraction."** The physics behind it is exact. A drop sitting on
concrete has *nothing behind it* - what you would "see through" the drop is the same concrete it is
sitting on - so a drop on an opaque surface has no refraction to do, and its entire visual signature is
specular: the environment reflected in a curved water lens, Fresnel-weighted, sharpened by the wetness.
That is why the opaque path needs no shading code of its own at all. It writes a perturbed normal, a
tightened roughness and a darkened albedo into the G-buffer, and the probes and lights that already run
downstream do the rest.

**Glass sits between the lens and the wall, and this spec should be read that way rather than as a
variant of either.**

- Unlike an opaque surface, **a pane genuinely does have something behind it.** Refraction is physically
  meaningful on glass in a way it is simply not on a wall: a drop on a window bends and magnifies the
  street outside. That distortion is most of what makes rain-on-a-window compelling, and it is very
  likely a large part of what the user was looking forward to. Denying glass refraction on the strength
  of a rule written about opaque surfaces would be applying the rule's letter against its own reason.
- Unlike the lens, **glass is world-anchored and arbitrarily oriented**, so its reflection cannot come
  from the presented frame the way the lens's rim highlight does. It must come from the **reflection
  probes** - which is already exactly how transparent materials get their specular in the forward pass.

**Decision: glass gets both - refraction of what is behind the pane, plus probe reflection on the drop's
own normal, Fresnel-weighted between them.** That is the standard water-surface split, it is what a real
drop on real glass does, and it is what the forward alpha pass is equipped to do.

**The reflection half is nearly free.** All three target paths already call the probe sampler with the
fragment position and the shading normal: `sampleReflectionProbesLegacy(...)` at
`class2/deferred/alphaF.glsl:260` and `class3/deferred/materialF.glsl:342`, and
`sampleReflectionProbes(...)` at `class2/deferred/pbralphaF.glsl:189`. The drop layer perturbs `norm`
*before* that call and the probe reflection follows automatically - no new sampler, no new binding, no
new probe code, no pass of its own. This is the concrete payoff of the section-2 argument that a forward
material shader has the surface normal and world position natively: the drop's normal perturbation
**composes into the material's existing specular path** instead of having to reconstruct one.

**The refraction half needs a copy of the scene behind the pane, and the frame already makes one.** A
forward shader cannot sample the framebuffer it is blending into, so refraction needs a backdrop texture
- and water, which is the same problem (a transparent, world-anchored, arbitrarily oriented forward
surface), already solves it: `LLDrawPoolWater::beginPostDeferredPass` copies `mRT->screen` colour plus
`deferredScreen` depth into `mWaterDis` through `gCopyDepthProgram` with one fullscreen triangle
(`lldrawpoolwater.cpp:118-147`) and binds it as `WATER_SCREENTEX` (`:272`). **Glass takes the same idiom
and the same buffer.**

Timing, checked rather than assumed. Pool order is `ALPHA_PRE_WATER`, `WATER`, `ALPHA_POST_WATER`,
`ALPHA`. The water pool's copy at the start of `WATER` writes colour **and** depth. `doAtmospherics`,
injected at `ALPHA_POST_WATER` (`pipeline.cpp:4474`, body at `:10367-10392`), re-copies into `mWaterDis`
- but with `setColorMask(false, false)` (`:10388`), so it refreshes **depth only** and leaves the water
pool's colour in place; its own comment says as much ("copy depth buffer for use in haze shader (use
water displacement map as temp storage)"). By the time post-water alpha draws its faces, `mWaterDis`
therefore holds a usable but *stale* colour: the scene as of just before water was drawn.

**Recommendation: take one fresh colour+depth copy into `mWaterDis` at the start of the post-water alpha
pool, modelled line-for-line on `LLDrawPoolWater::beginPostDeferredPass`.** Zero new render targets and
zero new memory - `mWaterDis` is already screen-format RGBA16F at screen resolution, and its colour is
dead by that point in the frame (`doWaterHaze` consumed it at `ALPHA_PRE_WATER`; `doAtmospherics` only
wants its depth). Cost: one fullscreen triangle per frame, and only when there is wettable blended
geometry to draw. The backdrop then contains the opaque world, pre-water alpha, the water surface and
the haze - everything a pane can meaningfully refract.

Two things to verify before building, and one stated limit:

- **Verify** that nothing after post-water alpha reads `mWaterDis`'s colour.
  `doc/viewer/frame_pipeline.md` calls it "triple-duty scratch: water refraction + depth scratch for
  atmospherics/haze"; both named consumers run earlier, but this must be checked in code, not inferred.
  If something does read it, fall back to a dedicated screen-format RT - one extra buffer, same shape,
  same per-frame cost.
- **Verify** the copy is taken unconditionally rather than under `sRenderTransparentWater`. Reusing the
  water pool's own copy would tie glass refraction to whether the scene has water at all, and the failure
  mode - stale garbage refracted through every window in a waterless region - is far worse than the cost
  of taking our own.
- **Stated limit:** the backdrop is the frame as of just before post-water alpha, so a pane refracts the
  opaque world, pre-water alpha and water correctly, and does **not** refract another pane behind it, nor
  the volumetric clouds, lightning or precipitation the SS weather block draws after the copy
  (`pipeline.cpp:4474-4494`). Glass behind glass refracts the opaque world. This is exactly the limitation
  water itself has lived with in this renderer for years and it is invisible in practice; recorded so it
  is not later diagnosed as a bug.

Refraction offset is the drop's normal perturbation projected into screen space and scaled by
`SS_GLASS_REFRACT_PX` (a few pixels for a 4 cm drop, scaled by the drop's on-screen footprint so it
fades out with distance exactly as the drop pattern itself does). Fresnel is Schlick on the
**perturbed** normal against the view vector, F0 the pane's own: at grazing angles the drop reflects the
sky and the refraction disappears, head-on it is mostly refraction with a bright rim - which is what a
drop on a window actually looks like.

**The asymmetry table. Three systems, three shading models, one drop law.** Recorded here because a
future reader will otherwise try to unify them:

| | Refraction | Reflection | State | Anchoring | Driven through |
|---|---|---|---|---|---|
| **Lens drops** (`ssPostLensF.glsl`) | of the **presented frame** - the drop is on the camera, the whole scene is behind it | Fresnel **rim** highlight off the screen; probes a documented follow-up | **Stateful** simulation - accumulating population, running heads, wake beads | Screen space; `slideDir` = gravity's screen projection + wind | Its own post pass |
| **Glass** (this doc) | of **what is behind the pane** - a backdrop copy of the scene, offset by the drop normal | **Reflection probes**, on the drop's own perturbed normal, riding the material's existing probe call | **Stateless**, world-anchored procedural law; wetness carried by the surface field | World space; lattice in the pane's own (tangent, world-Z) frame | The forward alpha material shaders |
| **Opaque surface** (`ssSurface*F.glsl`) | **None** - there is nothing behind a wall to refract | **Reflection probes only**, via the G-buffer normal the wet pass writes | **Stateless**, world-anchored; wetness from the same field | World space; same lattice | The G-buffer, then the existing lighting |

The drop law itself - release, run, stutter, trail, and the density and size `ssDropLaw` hands back - is
**one law** across all three rows. Only the shading model and the state model differ, and they differ
because the three surfaces genuinely are different things: a camera lens, a pane with a world behind it,
and a wall.

### 3c. Blend semantics, and which existing laws apply to a pane

**The pane's transparency must survive, and a drop must make it MORE visible.** The forward alpha blend
is `blendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ZERO, ONE_MINUS_SRC_ALPHA)`
(`lldrawpoolalpha.cpp:250-254`; `doc/viewer/glow_and_alpha.md`). Two consequences:

1. `frag_color.a` here is **real coverage**, not the bloom mask. The bloom-mask rule applies to
   post-deferred passes writing the screen target's alpha *additively*, and to the G-buffer's
   attachment-specific meanings - neither is this. So raising alpha inside a drop's footprint is a legal,
   correct thing to do, and it is exactly what a drop does: it makes the pane locally more opaque and more
   visible. Bound it: `SS_GLASS_ALPHA_LIFT = 0.30` at a drop crown, tapering to 0 at the drop's edge, and
   **zero everywhere the drop pattern is zero** - rung R6 measures the untouched texel.
2. The alpha destination factor is glow suppression: raising src alpha erases proportionally more of the
   glow already accumulated behind the pane. A glowing sign behind a rain-streaked window blooms very
   slightly less. That is correct and negligible; recorded so it is not diagnosed later as a bug.

**Which existing surface laws apply to a pane, and which do not.**

- **Porosity** (`ssPorosity` / `ssPorosityAt` / `ssPorosityFromAlbedo`): does not apply, and better than
  that, it is unnecessary. Its job (`doc/atmo_magic_surface_weather.md` section 4) is "to stop glass
  darkening like soil and soil shining like glass" - it exists to *estimate* sealedness from a G-buffer
  that carries no material identity. On the blended path there is nothing to estimate: everything people
  make transparent is sealed. **Hardcode porosity 0 on the glass path.** It saves the four albedo
  variance taps, it removes the pass's dependence on `diffuseRect` entirely, and it is more accurate than
  the estimate it replaces. Consequence: `ssWetAlbedoExponent` is 1, so wet **does not darken glass** -
  which is right.
- **Wetness** (`ssFieldAt(...).x`): applies unchanged. It is the one field channel a wall legitimately
  earns on its own account (`ssSurfaceFieldF.glsl:153`), and a pane is a wall.
- **Deposit** (`ssDepositMask`, coverage, translucency, sparkle): **not in this phase.** A snow layer
  replaces the albedo and drives alpha toward 1, which changes the pane's sorting and occlusion character
  and needs its own thinking. A snow-covered skylight is a stated gap, not an oversight. Section 9, question 6.
- **Standing water and the angle rails** `ssWetFlattenCosFull` / `ssWetFlattenCosZero`: apply, and they
  already answer glass correctly without a special case. `ssSurfaceLevel(n, cosZero, cosFull)` is a
  `smoothstep` on `dot(n, worldUp)`; the settings are `SSAtmoWetFlattenAngleFull` 10 deg and
  `SSAtmoWetFlattenAngleZero` 30 deg (`settings.xml:26829`, `:26840`), i.e. cos 0.985 and cos 0.866. A
  vertical pane reads `level = 0`, so the puddle flatten is off, the static-drop term is off (it gates on
  `slope_factor`, `ssSurfaceNormalF.glsl:257`) and the drip term is on - which is precisely the physics: a
  window holds no standing water, it holds runners. A horizontal pane (skylight, glass table top) reads
  `level = 1` and gets static drops, which is also right. **No new law is needed for "a pane holds no
  standing water"; the existing rail already says it.** One thing the glass path must NOT inherit is the
  puddle *depth*: `ssFieldAt` only returns `puddle` when the fragment is `on_top` of its own column
  (`ssSurfaceFieldF.glsl:155-156`), and a skylight that IS the column top would otherwise get a mirror
  puddle painted on a transparent surface. **Force puddle to 0 on the blended path**, unconditionally.
  (Side note found while verifying: the `LLCachedControl` fallbacks at `sssurfacefield.cpp:1722-1729` say
  25 and 65 degrees where `settings.xml` says 10 and 30. The settings file wins at runtime, so this is
  cosmetic, but the two should be made to agree.)
- **Ice and frost**: frost on a window is the single most recognisable cold-weather cue in the whole
  feature and the field already carries the channel. It is not in phase 1 only to keep the first landing
  small. Section 9, question 2 recommends it as the immediate follow-up.

## 4. Shelter and sidedness

The field already owns the entire answer and it costs one function call.

`ssFieldAt(vec3 p_agent, vec3 n_agent)` (`ssSurfaceFieldF.glsl:98-159`) returns `(wet, snow, puddle,
exposure)`, where exposure is 0 fully sheltered to 1 open sky, and **negative** where the window has no
answer at all. Its shelter authority is:

- The **column-ownership test** (`:106`): a fragment more than `cell * 1.5` above its own column's stored
  height gets "no answer". A pane in a wall sits below its own column top, so it takes the march. A pane
  on a moving object above the capture gets no answer and the layer switches off - correct, and it is the
  same guard that keeps avatars out.
- The **exposure march** (`:119-147`): from `p + n * (cell * 0.75)`, five rays across a cone around
  `ssFieldFall` (the agent-space normalised fall direction, the same axis `SSRainShadowMap` captured
  along), spread by `ssFieldSpread` (`SSAtmoWetSpread`, default 0.35), rotated by a per-fragment hash
  angle so the six-level answer dithers instead of banding, each ray 8 steps of `cell * 1.25`.
  `exposure = 1 - blocked * 0.2`. This is why a pane under an eave stays dry and a pane in the open wets.
- The **facing term** (`:151`): `facing = mix(1, clamp(dot(n_agent, -ssFieldFall), 0, 1), ssFieldFacing)`
  with `ssFieldFacing` from `SSAtmoWetFacing`, default 0.6. Driven rain wets the face it can see. In still
  air `ssFieldFall` is straight down and this does nothing to a vertical pane, which is correct: in still
  air a window does not get wet.

**Which face gets wet, and how the pass knows.** Not `gl_FrontFacing`. The weather-facing side is a
property of the pane and the sky, not of where the camera stands:

    vec3 n_w   = normalize((ssFieldInvView * vec4(vary_norm, 0.0)).xyz);
    vec3 n_out = (dot(n_w, -ssFieldFall) >= 0.0) ? n_w : -n_w;   // the side the weather sees

Build the whole layer - the exposure query, the drip lattice frame `drip_t = cross(worldUp, n_out)`, the
cell hashes - from `n_out`. The pattern is then **identical from both sides**, because nothing in it
depends on the viewer. That is both physically right (you see the outside drops through the glass from
indoors) and the cheapest possible guarantee of camera invariance. Rung R3 pins it.

**A pane seen from the dry side** still shows the drops, attenuated, because you are looking through the
pane at them: multiply the layer's visual weight by `SS_GLASS_FAR_SIDE = 0.75` when
`gl_FrontFacing != (dot(n_w, -ssFieldFall) >= 0.0)`. This is the one and only place `gl_FrontFacing` is
allowed to enter, it modulates strength and never geometry, and it is why the drops do not pop when you
walk through a doorway - 0.75 to 1.0, not 0 to 1. A double-sided GLTF pane
(`mGLTFMaterial->mDoubleSided`, `lldrawpoolalpha.cpp:531`) is drawn twice with flipped facing; because
the pattern is built from `n_out` and not from the facing, both draws produce the same drops in the same
world places, and only the 0.75 factor differs.

Condensation on the *inner* face - the other half of the sidedness story, and a beautiful look - is a
follow-up (section 9, question 3), and it is the one place where `gl_FrontFacing`-derived sidedness genuinely
matters, because condensation forms on the warm side.

## 5. Where the shared include gets its inputs

For the record, so the phase does not rediscover it: the glass include needs `ssFieldMap` +
`ssFieldOrigin` + `ssFieldFall` + `ssFieldSpread` + `ssFieldFacing` (for `ssFieldAt`), `ssFieldInvView`,
`ssSurfaceIntensity`, `ssTime`, `ssSurfaceWind`, `ssSurfaceDrops`, `ssSurfaceDropScale` and the flatten
rails; plus, for the refraction half of section 3b, the backdrop sampler (`mWaterDis`, bound on
`WATER_SCREENTEX` exactly as water binds it at `lldrawpoolwater.cpp:272`) and `screen_res` to turn the
drop's normal perturbation into a pixel offset. It does **not** need `diffuseRect`, `specularRect`,
`ssFieldFlowMap`, `ssFieldStateMap`, `ssWaveMap`, `inv_proj` or any depth-reconstruction machinery -
the backdrop is sampled at the fragment's own `gl_FragCoord`, offset, and nothing is unprojected. That
is still a materially smaller binding than any of the three G-buffer passes carries. A new
`SSSurfaceField::bindForwardForShader(LLGLSLShader&)` uploads exactly that set, is called from
`LLDrawPoolAlpha::prepare_alpha_shader` alongside the existing per-shader setup
(`lldrawpoolalpha.cpp:166-196`), and is simply not called for the seven excluded programs. Every uniform
any linked unit reads must be uploaded per program even when the host file's own text never mentions it
- the AUDIT finding 10 rule.

## 6. The ladder

Each rung with the control that must make it fail. **R4 is the central rung**, not one of several: the
whole design rests on a stateless, world-anchored law (section 3a), and camera invariance is exactly the
property that being stateless and world-anchored is *for*. If R4 is green the design is doing its job;
if R4 is red nothing else on this list is worth reading.

- **R4 - Camera invariance (the central rung).** Place a marker at a fixed world point on a vertical
  pane. Orbit 360 degrees; dolly 2 m to 30 m; roll the camera; look away until the pane is culled and look
  back. The drip lattice cell containing the marker must not change, the drop at that cell must not jump,
  and the pattern after the pane re-enters view must be **continuous with** what it would have been had
  the pane never left - which is a property a stateless law gets for free and a stateful one cannot get at
  all. *Failing control:* replace `dot(p_world, drip_t)` with a view-space or screen-space coordinate; the
  drops swim. Second failing control: introduce any per-pane accumulator seeded on first sight; the
  re-entry test goes red. This rung exists even though the forward path is structurally immune to the
  depth-reconstruction defect currently observed on the masked path, because `ssFieldInvView` is still in
  the chain and a stale or wrong inverse-view would reintroduce it.
- **R1 - Twin.** `V:\Scratch\surface\tests\twin_glass_glsl.cpp` transliterates `ssSurfaceGlassF.glsl`
  against `sssurfacestatecore.h`, LOCKSTEP constants read from both sources, golden pins at seed
  0x5EED1337. *Failing control:* perturb one `SS_DROP_*` constant in the GLSL by 1e-3; the twin must go
  red.
- **R2 - Uniform-bind completeness.** A debug list of every uniform the include declares against what
  `bindForwardForShader` uploaded, asserted once per program on first bind. *Failing control:* comment
  out one `uniform1f` call; the check must name that uniform. (This exact bug class already happened -
  `sssurfacefield.cpp:2104`.)
- **R3 - Sidedness invariance.** One pane, one wall, rain from one direction. Photograph the drop
  pattern from outside, walk through the door, photograph from inside. The drops must be in the **same
  world positions**. *Failing control:* build `drip_t` and the lattice from `gl_FrontFacing ? n : -n`
  instead of `n_out`; the pattern mirrors when you walk through and the rung goes red.
- **R5 - Shelter.** Two identical panes on the same wall, one under a 1 m eave, one in the open, in
  driven rain. Open wets, sheltered stays dry, and the boundary between them is dithered rather than a
  hard line. *Failing control:* force `ssFieldFacing` to 0 and `ssFieldSpread` to 0; both wet and the
  penumbra disappears.
- **R6 - Transparency survives.** A pane at 20% alpha. Read back framebuffer alpha at a texel with no
  drop on it: it must equal the stock build's value to 1e-3. At a drop crown it must be higher by at most
  `SS_GLASS_ALPHA_LIFT`. *Failing control:* apply the lift as a flat term across the pane; the no-drop
  texel differs and the rung goes red.
- **R7 - Off is bit-identical.** With `SSAtmoEnabled` off, and separately with `SSAtmoSurfaceDrops` at 0,
  the pane's pixels must be bit-identical to a build without the include. *Failing control:* leave any
  term that is not exactly a no-op at zero strength (a `mix(x, y, 0.0)` through a normalize, for
  instance).
- **R8 - Order.** Two panes at 30 degrees to each other, both wet, one behind the other. Drops on the far
  pane must be occluded by the near pane's own drops and tint in the pool's own sort order. *Failing
  control:* this rung is passed by construction under option (a) and **fails by construction** under (b)
  and (c) - which is the rung to point at if anyone reopens that choice.
- **R9 - No-answer regions.** A pane beyond the field window (over ~256 m from the camera on a 2 m-cell
  region; `WINDOW_RES` 256), and a pane on a moving vehicle above its column top. The layer must be
  exactly off in both, with no flicker as the window's snapped origin steps a cell
  (`sssurfacefield.cpp:1227`). *Failing control:* return exposure 1 instead of the negative sentinel on
  the out-of-window branch; the pane flickers wet at the boundary.
- **R10 - Refraction and Fresnel (section 3b).** A pane in front of a strongly patterned wall
  (checkerboard). Head-on, the drops must visibly displace the pattern behind the pane and the
  displacement must scale down with distance as the drop footprint shrinks. Rotated to grazing, the
  displacement must give way to a sky reflection. *Failing controls, three:* (i) hold the drop normal
  fixed and only sample the backdrop - the displacement stops tracking the drop and the rung goes red;
  (ii) set the Fresnel term to a constant - the grazing view keeps refracting and stops reflecting;
  (iii) suppress the backdrop copy for one frame and confirm the pane refracts a stale frame visibly,
  which is what the "verify nothing else reads `mWaterDis`" item is guarding against - this control must
  be *seen to fail* before the reuse is trusted.
- **R11 - Probe reflection rides the host path.** With the drop layer on and the refraction term forced
  to zero, a drop must still catch the environment: move a bright probe-lit object and the drop
  highlights must move with it. *Failing control:* perturb `norm` *after* the `sampleReflectionProbes`
  call instead of before it; the drops go flat and the rung goes red. This rung is what proves the layer
  composed into the material's existing specular path rather than bolting a second one beside it.

## 7. Cost

Per fragment, on a wettable blended fragment only: one `ssFieldAt` - which is `exposure = 1.0` and a
single texture fetch on the common "top of its own column" path, and up to 5 rays x 8 fetches in the
sheltered-wall path (the same cost the opaque wall path already pays); one `ssDropLaw` (five
`smoothstep`/`mix`, no taps); up to four `ssFieldHash` calls for the drip cell and up to four for a
static-drop cell, each one `dot`/`sin`/`fract`; two `fwidth` pairs for the footprint fade; **one**
backdrop tap for the refraction, taken only where the drop coverage is non-zero; and the probe
reflection, which is not a new cost at all because it is the material's existing probe call reading a
perturbed normal. No `diffuseRect`/`specularRect` taps and no depth reconstruction at all. All of it
behind `drop_gate > 0.001 && drip_reach > 0.001` early-outs, so a dry world or a distant pane costs one
field fetch and a branch. Per frame: **one fullscreen triangle** for the backdrop copy (section 3b),
skipped entirely when there is no wettable blended geometry. Zero extra draw calls for the geometry,
zero extra vertex work, zero new render targets, zero new memory. This is a genuinely cheap feature; the
expensive-looking part - the exposure march - is per-fragment work the wall next to the window is
already doing.

Code cost: one new shader file, five stock shader files gaining a prototype block and one call each
(behind `SS_ATMO`), one new `bindForwardForShader` in `sssurfacefield.cpp`, one call from
`lldrawpoolalpha.cpp:166-196`, twelve `mShaderFiles` entries in `llviewershadermgr.cpp` (four loops, not
twelve edits), two or three new `SSAtmo*` settings, one new twin.

## 8. Order, and whether it lands independently

**It lands independently.** This work touches none of the files backlog item 4.14 (surface weather phase
3) owns - `ssheightfog.*`, `ssscreenfx.*`, `ssPostFogF.glsl`, `ssPostHeatF.glsl`, `ssPostLensF.glsl` -
and none of the three G-buffer pass shaders. 4.14 is DESIGNED-NOT-BUILT on a separate branch and blocked
on the phase-2 build verdict (`doc/atmo_magic_backlog.md:422-425`); this is blocked on nothing of
4.14's. The two can proceed in parallel without a merge conflict, with one exception: the `dripRun`
hoist proposed in section 3a touches `ssPostLensF.glsl`, so that hoist is sequenced *after* 4.14, not
inside this.

**But it should not START yet, and the reason is not scheduling.** The masked path's drop anchoring is
being fixed right now (`ssSurfaceWetF.glsl` / `sssurfacefield.cpp`), and the glass include transliterates
the same drop and drip law. Transliterating a law that is about to change is how you get two sites that
disagree - the precise failure the ONE FORMULA SITE rule names. **Recommendation: land the anchoring fix,
get the user's confirmation that the drops sit still on masked and opaque surfaces, then start this as
its own small phase, ahead of 4.14.**

Ahead of 4.14 because: it is what the user was most looking forward to; it is a day-scale change against
4.14's "large"; it needs no new subsystem, no new render target and no new post pass; and every hour it
waits is an hour the most-anticipated look is missing from a feature the user is otherwise ready to sign
off.

**The cheap 80%, stated plainly.** If even this is too much, the version that gets most of the look for
about a third of the work is: `class2/deferred/alphaF.glsl` **only** (the legacy Blinn-Phong blended
path, which is exactly the transparency-slider prim the user described), drips only (no static drops, no
sheet, no ice, no frost, no deposit), normal perturbation plus the alpha lift, **probe reflection but no
refraction** - and note which half gets cut, because it is the cheap one that survives: the probe
reflection is free (it is the host's existing call reading a perturbed normal), while the refraction is
what costs the backdrop copy and the extra tap. That is one shader file, two programs, one twin, no new
frame-order work at all, and it puts runners on the windows of the overwhelming majority of SL glass.
The full five-file version adds material and GLTF glass, horizontal panes, the refraction of what is
behind the pane, and the Fresnel split that makes a drop read as a lens rather than as a bump.
Recommendation: build the full five-file version - the incremental cost over the 80% is small and it is
all plumbing, not new law - but if the phase has to be cut, cut it to the 80% rather than deferring the
whole thing, and add the refraction back as the first follow-up because that is the part the user was
most looking forward to.

## 9. Open questions for the user

1. **Fullbright glass.** A large share of SL "glass" is fullbright as well as transparent, and fullbright
   is a *forward* pool even when opaque (`lldrawpoolsimple.h:101`, `:119`), so `fullbrightF.glsl` misses
   the weather entirely - transparent or not. Including it is one more file and two more programs.
   *Recommendation: include it, and treat a fullbright surface as taking the drop layer's normal
   perturbation and alpha lift but no lighting term, since it has no lighting. It is cheap and it closes
   a gap that is not really about glass at all.*
2. **Frost on windows.** The field already carries the frost channel. Frost on a pane is an alpha lift
   plus a white grain plus a desaturation - a few lines on top of everything this design already builds.
   *Recommendation: yes, immediately after phase 1, as its own commit. It is the highest look-per-line
   item in the whole surface-weather feature.*
3. **Condensation on the inside face.** The lens pass already has a condensation law
   (`ssscreenfxcore.h:131-134`, `LENS_FOG_COLD_C 8`, `LENS_FOG_WARM_C 20`). A cold pane with warm humid
   air behind it fogs on the warm side and the runners cut it clear - the same shape, applied to geometry.
   *Recommendation: follow-up after frost, and hoist the law into `sssurfacestatecore.h` when doing it so
   the lens and the pane share one site.*
4. **How opaque should a drop make the pane?** `SS_GLASS_ALPHA_LIFT` at a drop crown.
   *Recommendation: 0.30, tapering to 0 at the drop edge, with a setting so it can be tuned by eye. Too
   high and a rainy window stops being a window; too low and nothing reads at distance.*
5. **The backdrop buffer for refraction** - section 3b recommends refreshing `mWaterDis` with a fresh
   colour+depth copy at the start of the post-water alpha pool, on the grounds that its colour is dead by
   then and it is already the right format and size. That saves a full screen-resolution RGBA16F render
   target. *Recommendation: reuse `mWaterDis`, but only after confirming in code that nothing later in the
   frame reads its colour; if anything does, take a dedicated RT rather than trying to be clever about
   ordering. The whole gain here is one buffer's worth of VRAM and it is not worth a subtle frame-order
   bug.*
6. **Deposits (snow) on horizontal glass** - a snow-covered skylight. *Recommendation: leave it out. It
   needs alpha to go to 1, which changes the pane's sorting and occlusion character; it is a small feature
   with a large blast radius and should be its own decision.*
7. **Alpha clothing and hair on avatars** - they go through `alphaF.glsl` too. *Recommendation: leave
   them to `ssavatarwet` by not binding the field for `gDeferredAvatarAlphaProgram`. Two models fighting
   over one fragment is the bug `ssSurfaceFieldF.glsl:106` was written to prevent.*
