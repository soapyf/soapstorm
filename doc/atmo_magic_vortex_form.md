# Atmo Magic: the funnel is made of cloud — vortex form, phase 8h

Status: **design** (2026-09-06, from the user's verdict on the third build's tornado). Written against
`doc/atmo_magic_phase8_show.md` section 4·0 (the deck is a container, entities are operators on it), which already
decided the shape of this change; against `doc/atmo_magic_far_clouds.md` section 3 (the rain shafts, the precedent for
emitting an entity's geometry into the deck's own puff list); and against `doc/atmo_magic_storm_dynamics.md` section 4
(the vortex taxonomy, which is unaffected — nothing here touches which vortices exist, only what they are made of).

The user's verdict, verbatim, because every decision below answers one clause of it:

> a closer look at the tornado... it is looking quite flat, the debug circles have got more dimension and substance
> than the actual tornado renderer. The current renderer sort of works if we are really far away and are looking at it
> head on, as the lowest LOD, but at any other angle and close up it looks shit. Also its not rendered the same way as
> the clouds are at all, it should be using same shader as clouds, because the tornados look like the clouds, just a
> tad darker perhaps.

---

## 1. The verdict and the diagnosis

### 1.1 The funnel is one flat ribbon, and that is not a figure of speech

`SSVortexRender::render()` walks the funnel's `SSVortex::COLLARS` (14) collar rows, subdivides each span for the
squash (`ssvortexrender.cpp:541`), and for every subdivision step emits **two triangles between exactly two vertices'
worth of horizontal extent**: `left_pos = axis_pos - right * cardHalfWidthM(radius)` and
`right_pos = axis_pos + right * cardHalfWidthM(radius)` (`ssvortexrender.cpp:602-608`, quads at `:612-626`). There is
no second span, no ring, no back face, no interior. The whole tornado — top to contact — is **a single planar strip,
yaw-billboarded about world +Z**, one polygon wide.

The file's own comment calls this "Z-billboarded collar quads", and the plural reads as though there were a stack of
rings. There is not. `COLLARS` indexes rows *up the strip*, not cards *around the axis*. A funnel of any width, at any
intensity, is one ribbon whose width happens to follow `collarRadius`.

This single fact explains every clause of the verdict:

- **"Flat."** It is flat. It has zero thickness by construction. Nothing in the pipeline could make it read otherwise.
- **"Really far away and head on... as the lowest LOD."** A yaw-billboarded ribbon is exactly a correct impostor for a
  rotationally symmetric column *seen from far enough that the column's own depth is below an arcminute*. The current
  renderer is not a broken funnel renderer; it is a correct **lowest-LOD impostor with no other rungs above it**. The
  user diagnosed it precisely.
- **"At any other angle... it looks shit."** From above, from below, from inside the reach, the ribbon shows its
  degenerate case: a triangle-ish sheet with a silhouette but no volume, sliding its yaw as the camera walks.
- **"The debug circles have got more dimension."** The V2 overlay draws "collar rings: the funnel's own radius/altitude
  table, wireframe" — actual circles at actual altitudes. Fourteen wireframe circles genuinely describe more of a
  three-dimensional solid than one flat quad strip does, so they read as more substantial. That is not an insult to
  the overlay; it is the measurement.

### 1.2 The shading has no relationship to the deck's

`ssVortexF.glsl` carries the sky's windlight uniforms (`sunlight_color`, `ambient_color`, `:47-48`) and then spends
them on a model of its own invention:

```
vec3 base   = vec3(0.15, 0.16, 0.20);            // ssVortexF.glsl:162 - a hardcoded grey-blue
vec3 shaded = (amb * 0.6 + sun * 0.9) * mix(1.0, ss_gloom, 0.6);   // :165
```

Against `ssVolCloudF.glsl`, what is *missing* is the entire deck:

| deck term | in the funnel? |
|---|---|
| the triplanar cloud noise body (`n_map`, the three-plane read at world position) | **no** |
| `ss_sphere_normal` — the fake-sphere normal every puff and every shaft reconstructs | **no** |
| `SSDeckShade::form` / `buried` — the structural shading core the veil, the fine puff and the macro body all share | **no** |
| the wrap light, sun facing and graze lid | **no** (one CPU `facingLight` per corner, `ssvortexrender.cpp:595`) |
| `vary_ss_sunlit` / `vary_ss_amblit` — the dome band's own colours the deck is shaded by | **no** |
| `vary_ss_airlight` — the below-cloud airlight added after every cloud multiplier | **no** |
| `vary_ss_glow` — the sky's forward-scatter, gated by per-fragment thinness | **no** |
| the storm gloom graded over a **buried depth** | **no** — a flat 60 % blend, and the shader says so |

The shader's own comment at `:159-161` states the reason for the last row plainly and correctly: *"the funnel is not a
puff with a buried depth to grade over, so gloom is spent as a flat 60 % blend toward it rather than SSVolCloud's own
buried-depth gradient, which has no equivalent here."* That is an honest comment about a wrong design. The funnel has
no buried depth **because it is not made of deck**. The user's "it should be using same shader as clouds" is the
correct reading of the same sentence from the other end.

### 1.3 It draws with no deck, and that is the same bug

`ssvortices.cpp:415`:

```
const F32 wall_cloud_z = SSVortex::wallCloudAltitudeM(ground_z, applier->windProfileBaseZ());
```

The funnel's top altitude comes from the **resolver's** deck base — an environment scalar that is defined on a
perfectly clear day — and not from `SSVolCloud::cloudBaseZ()`. That choice is itself right and must not be reverted:
its comment (`ssvortices.cpp:411-414`, SCHEDULER fix 4) records that `SSVortices::update()` can run before
`SSVolCloud::update()`, so reading the live deck would read *last frame's* build. But nothing anywhere else in the
vortex path consults the deck either. Not `childVortex`, not `vortexAt`, not `SSVortexRender::render()`. **No code path
between the storm lattice and the funnel's pixels asks whether a single puff exists at that location.**

So a funnel condenses out of a blue sky, which is what the first screenshot shows.

This is not a second defect to fix separately. A tornado's funnel *is* condensation — the visible boundary where the
pressure drop takes the air below its dew point, hanging from a cloud base that is itself the same boundary at a
different altitude. When the funnel is made of the deck's own puffs, "no deck here, no funnel here" is not a gate
anyone has to write and keep in lockstep. It is arithmetic: no puffs at the location means nothing to displace and
nothing to shade like. **One change fixes both.**

---

## 2. The model

Section 4·0 already decided this, and this document is not free to invent a parallel scheme. Restating it in its own
words, since it is the specification:

> **The tornado, in these terms:** the funnel itself stays an emission (the collar quads, the vortex core's shape and
> lifecycle). What changes is that the deck's puffs near the axis are *operated on*: a swirl about the axis whose angle
> falls off with radius and vanishes at the reach, an inward radial pull, and a downward pull strongest near the axis
> and near the base, so the wall cloud and the lowering are the deck's own fabric being drawn into the funnel's top
> rather than a separate object hung beneath it.

This design keeps that word for word, with **one amendment the verdict forces**: where 4·0 says the funnel "stays an
emission (the collar quads)", the collar quads go. The emission stays; its *geometry* becomes puffs. That is the same
amendment the shafts already made to their own first design, for the same reason (§3.3 below), and it is the difference
between an entity that emits into the container and one that emits beside it.

The funnel is therefore **two operators**, both on the deck:

- **an emission operator** — funnel bodies, ordinary `SSVolCloud::Puff`s in `deck.mPuffs`, laid on rings up the axis;
- **a displacement operator** — `D(p, t)`: swirl + inward + down, applied to every ordinary deck puff inside the
  vortex's reach, and inverted by every observer of the field.

### 2.1 The bodies, and the LOD law

**Placement.** Bodies sit on **rings** at **rows** up the funnel. A body's polar coordinates in the funnel's own frame
are `(h01, θ)`; its world position is

```
axis_xy(h01) = contact_xy + tiltDir * SSVortex::tiltOffsetM(tiltFrac, h01, heightM)     // existing, unchanged
z            = SSVortex::collarAltitudeM(groundZ, wallCloudZ, h01)                      // existing, unchanged
r(h01, θ)    = SSVortex::collarRadius(h01, intensity, taper, condensation)              // existing, unchanged
             * SSVortex::radiusModulation(θ, t, multiN, multiOmega, multiPhase, multiWeight)  // existing, unchanged
pos_xy       = axis_xy(h01) + r * (cos θ, sin θ)
```

**Four existing core functions, called with the arguments they were written for. No second profile is introduced, and
none may be.** `collarRadius` takes a *continuous* `h01` — the `COLLARS = 14` table is a sampling convention of the
render loop, not the profile — which is what makes the LOD law below exact rather than approximate.

`radiusModulation` moves from being a fragment-side alpha mask to being a **CPU placement term**, which is where the
core's own comment always said it belonged (`ssvortexcore.h:607`, "a SHADER term (GLSL twin)" is the sentence that
becomes false and improves by becoming false). The suction vortices stop being a smoothstep carved out of a padded card
and become the actual lumpy silhouette of a ring of bodies whose orbit radius breathes with `sin(N·θ + ω·t + φ)`. **The
GLSL twin retires.** One formula, one site, CPU only.

**The LOD law.** Rows and azimuths are **dyadic and nested**, so that changing LOD level never moves a surviving body —
it only adds or removes bodies. This is lesson 18 (a budget trim over a camera-bounded set must never be an index or a
rank) applied to a structured set, and it is stricter than a hash threshold because the nesting is exact:

```
rows(L) = 2^L + 1                     // h01 at i / 2^L, i in [0, 2^L] — level L is a strict subset of level L+1
az(A)   = 2^A                         // θ_j = 2π * vanDerCorput_2(j) — level A is a strict subset of level A+1
```

`vanDerCorput_2` (bit-reversal of the index) is what makes the azimuth nesting exact: the first `2^A` terms of the
sequence are the `2^A` equally spaced angles, for every `A`. Doubling the ring count keeps every existing body at its
existing angle and interleaves new ones between them.

The levels come from distance and from the funnel's own thinness:

```
L_dist = BODY_ROW_L_MAX - floor(log2(clamp(d / BODY_LOD_NEAR_M, 1, 2^(L_MAX - L_MIN))))
L_thin = ceil(log2(heightM / (2 * BODY_ROW_FILL_K * rMinM)))            // enough rows to close a narrow rope
L      = clamp(max(L_dist, L_thin), BODY_ROW_L_MIN, BODY_ROW_L_MAX)
A      = clamp(BODY_AZ_A_MAX - floor(log2(clamp(d / BODY_LOD_NEAR_M, 1, 2^(A_MAX - A_MIN)))), A_MIN, A_MAX)
```

with, as the named rails:

| constant | value | why |
|---|---|---|
| `BODY_ROW_L_MIN` / `L_MAX` | 1 / 4 | 3 rows at the far rung, 17 at the near one |
| `BODY_AZ_A_MIN` / `A_MAX` | 0 / 3 | **1** body per row far (the old ribbon's honest replacement), 8 near |
| `BODY_LOD_NEAR_M` | 300 | inside this, full detail |
| `BODY_LOD_FAR_M` | 4800 | `NEAR * 2^4`; the last halving lands here |
| `BODY_CAP_PER_FUNNEL` | 96 | enforced by dropping `A` first, then `L`; `17 × 8 = 136` is the unclamped worst case |
| `BODY_FLOOR_PER_FUNNEL` | 3 | `rows(1) × az(0)`; a live funnel is never zero bodies while it has deck to be made of |
| `BODY_ROW_FILL_K` | 0.6 | a row's bodies must reach 0.6 of the row spacing, or the column beads |

The floor is the answer to "the current renderer works as the lowest LOD": at three bodies the funnel *is* a smudge
seen through 5 km of haze, and it should be. The old ribbon was that smudge at every distance.

**Body radius.** Sized so a ring covers its own circumference, capped so a one-body ring is a column and not a balloon:

```
rBody = clamp(BODY_OVERLAP_K * π * r(h01) / az,
              BODY_ROW_FILL_K * rowSpacingM,        // vertical continuity floor
              BODY_RADIUS_FRAC_MAX * r(h01))        // never fatter than 1.25 collar radii
```

`BODY_OVERLAP_K = 1.35`, `BODY_RADIUS_FRAC_MAX = 1.25`. This is Tier B's coverage-conservation idea (`far_clouds`
§2 step 2) applied around a circle instead of over a macro-cell, and it is why the LOD collapse is graceful: at `az`
1 the single body is 1.25 collar radii wide and reads as the whole column.

**Stated residual, not hidden:** when the two clamps conflict — a thin rope-out drill at a coarse row level — the upper
clamp wins and the column reads as a bead chain rather than a continuous tube. Vertical continuity is therefore claimed
(and tested) for `taper >= 1` (wedge and ordinary funnels) and **explicitly not claimed** for the drill taper in
`PHASE_ROPE`. A roping tornado photographs as a bead chain anyway, and the debris skirt sits under the worst of it.

**Crossfade.** Level transitions crossfade the highest sub-index's alpha across the band, the same way Tier A does
across its `puffs_per_cell` edges (`far_clouds` §2 step 2) — one shared scalar read once, not two distances (the phase
6d finding that the crossfade read the block's centre distance in one half and the body's own 3-D distance in the
other, so both tiers drew at full weight along the band's inner edge, is the exact mistake to not repeat here).

### 2.2 The swirl: the column visibly rotates

The condensation funnel is the vortex **core**, and a Rankine core is in near solid-body rotation. So the whole column
shares one angular rate — no vertical shear in ω, which is both what a photograph shows and what keeps the column from
tearing itself apart between rows.

```
SSVortex::bodyOmegaRadS(intensity, taper, rotSign):
    vTan  = lerp(BODY_TAN_SPEED_MIN_MS, BODY_TAN_SPEED_MAX_MS, intensity)     // 25 .. 110 m/s
    rRef  = collarRadius(BODY_OMEGA_REF_H01, intensity, taper, 1.0)           // 0.5 — the SAME table, again
    return rotSign * clamp(vTan / max(rRef, WIDTH_MIN_M),
                           BODY_OMEGA_MIN_RAD_S, BODY_OMEGA_MAX_RAD_S)        // 0.05 .. 1.5 rad/s
```

At intensity 1, taper 1: `rRef = 900 * 0.54 = 486 m`, `ω = 0.226 rad/s`, 27.8 s per revolution, rim speed 110 m/s. At
intensity 0.3: `rRef = 153 m`, `ω = 0.33 rad/s`, 19 s per revolution. Tighter vortices spin faster in angle at
comparable circulation, which the formula gets for free because it divides by the funnel's own radius rather than
lerping an angular rate directly. The clamp exists for the degenerate tail (`WIDTH_MIN_M` at intensity 0) and is a
rail, not a tuning knob.

A body's angle is `θ_j(t) = 2π · vanDerCorput_2(j) + ω · t`. `t` is the **shared wall clock**
(`SSAtmoMagic::sharedTime`, the same clock `ss_time` already carries into `ssVortexF.glsl`), because this is
*animation*, not state — phase 8 section 2's rule, and lesson 31's.

**The noise must rotate with it.** If the bodies orbit through a world-fixed triplanar noise field, they slide through
a static texture and the column reads as a carousel of stencils rather than as turning cloud. So the funnel branch of
`ssVolCloudF.glsl` rotates its noise sample coordinate about the funnel axis by `−ω·t` before the triplanar read. This
is the same trick with the same justification as the deck's own advected detail (S2/S3), and it needs the axis position
and ω in the fragment stage — which the displacement operator below needs anyway, on the same uniform slot array.

### 2.3 The wall cloud and the lowering are displaced deck, not new geometry

`D(p, t)` in cylindrical coordinates about the funnel axis, with `q = (r, θ, z)` the puff's position relative to
`axis_xy(z)`:

```
w_r(r) = 1 - smoothstep(0, 1, r / reachM)                       // 1 on the axis, 0 at the reach, C1 at both ends
w_z(z) = smoothstep(topZ + PULL_Z_FADE_M, topZ, z)              // 1 at and below the deck base, 0 well above it
w      = w_r(r) * w_z(z)

r' = r - PULL_IN_MAX_FRAC * reachM * w_r(r) * w_z(z)            // inward
θ' = θ + ω * t * w                                              // swirl
z' = z - PULL_DOWN_M * w_r(r)^2 * w_z(z)                        // down: strongest on the axis and at the base
```

with `reachM = PULL_REACH_K * collarRadius(1, ...)` — **the funnel's own top radius, from the same table again**;
`PULL_REACH_K = 3.0`, `PULL_IN_MAX_FRAC = 0.22`, `PULL_DOWN_M = 220`, `PULL_Z_FADE_M = 400`.

Three properties make this safe, and each is a rung in §5:

1. **Bounded.** `|Δxy| ≤ 2r + PULL_IN_MAX_FRAC · reachM ≤ 2·reachM(1 + 0.11)`, `|Δz| ≤ PULL_DOWN_M`. A rotation is
   bounded in displacement (`2r sin(θ/2) ≤ 2r`) however long `t` runs, which is why the swirl may be unbounded in
   *angle* and still obey the frame rules — the same argument section 4 (original) makes for the supercell swirl.
2. **Exactly invertible in closed form.** This is the whole reason for choosing cylindrical coordinates. `θ' = θ + f(r)`
   preserves `r`, so its inverse is `θ = θ' − f(r')`: exact, no iteration. `r' = r − g(r)` is invertible iff `g'(r) < 1`
   everywhere, which is where `PULL_IN_MAX_FRAC = 0.22` comes from: `w_r` has slope magnitude at most `1.5/reachM`
   (the smoothstep's peak derivative), so `g' ≤ 0.22 · 1.5 = 0.33 < 1`. Likewise `∂(PULL_DOWN_M · w_r² · w_z)/∂z < 1`
   requires `PULL_DOWN_M · 1.5/PULL_Z_FADE_M < 1`, i.e. `220 · 1.5 / 400 = 0.825 < 1`. **Both margins are thin enough
   to be worth a test and thick enough to be real.** Raising either constant past its bound folds the field and the
   round-trip rung fails — which is exactly the failing control.
3. **Vanishes at the reach.** `w_r(reachM) = 0` with zero derivative, so the shear ring problem (lesson 10) does not
   arise: there is no discontinuity at the boundary for a ring of deck to tear along.

**The producer/observer rule, written before the call sites (lesson 12).** Composition order is 4·0's, and this
document does not get to change it:

```
producer (buildDeck placement):   hero shift  →  swirl  →  tornado pull  →  O(z)
observer (any read of the field): undo O(z)   →  undo tornado pull  →  undo swirl  →  undo hero shift
```

The observers are the four the replication set always means: the builder itself, `ssVolCloudF.glsl`'s fragment gate,
`bakeGroundShadow`'s texel loop, and `precipNoiseAt`. **The gate and any presence read are the same exception they
already are** — they are base-anchored by definition and read at `gate_air` (`ssVolCloudF.glsl:914-918`), and the
tornado pull is a *placement* displacement like O(z), not a re-classification of which column grew the puff. Say it in
the contract header, not only here.

### 2.4 "A tad darker perhaps"

No new colour. No new constant. The funnel bodies carry `mBuried = SSVirga::BURIED` (**1.0**), and everything else
follows from the deck's own shading.

The reasoning is the D2 finding's, reused rather than re-derived. `ssvirgacore.h:57` records that a virga curtain's
buried depth is 1.0 and deliberately **not** in lockstep with `Deck::SHEET_BURIED`, because "a curtain hangs BELOW the
deck with the whole column over it (1.0 is honest), while the veil sits INSIDE the deck's own floor band" and now
grades at `SSDeckShade::VEIL_DEPTH` (0.65). A tornado's funnel is the more extreme case of the same geometry: it hangs
below the wall cloud, which hangs below the deck, with *everything* over it. 1.0 is the dark extreme of the range the
gloom already grades across, and the fragment stage already knows exactly what to do with it.

`mForm` comes from `SSDeckShade::puffForm(sunZ, th, up, cellHeight, coreness, rim, beam)` evaluated at the body's own
altitude in the deck column, exactly like every fine puff and every macro body. The lit rim the old shader painted by
hand from a per-corner CPU dot product is deleted; the deck's `ss_sphere_normal` reconstruction, wrap light, sun facing,
graze lid, strike lighting, airlight and glow all arrive for free by being in the same shader.

The consequence the user asked for falls out without being asked for: the funnel is a *tad* darker than the cloud
around it — one buried step from 0.65 to 1.0 through the same gloom gradient — and it gets darker exactly when the
storm does, because it is graded by the same `ss_gloom` on the same curve as the belly of the deck it hangs from. If
the user wants it darker still, **the dial is the gloom, not a colour**, and that dial already exists.

---

## 3. What replaces `ssVortexF.glsl`

### 3.1 What dies

- `app_settings/shaders/class1/deferred/ssVortexF.glsl` — **deleted** (169 lines).
- `app_settings/shaders/class1/deferred/ssVortexV.glsl` — **deleted** (63 lines). Its far-field squash is
  `ssVolCloudV.glsl`'s, verbatim; a funnel body drawn as a puff gets it from the puff path.
- `gSSVortexProgram` and its whole registration block — `llviewershadermgr.cpp:188, 470, 1259, 2125-2141` — **deleted**.
- The collar draw loop in `ssvortexrender.cpp` (the `to_draw` build, the uniform upload, and the emission at
  `:490-641`) — **deleted**. The `SSVortexRender::render()` call in `pipeline.cpp` stays only for the debris skirt.
- `SSVortex::cardHalfWidthM` and `MULTI_RADIUS_EDGE_UNITS` — **deleted**. They exist solely to pad a card for a
  fragment mask that no longer exists. Every comment in `ssvortexcore.h` and `ssVortexF.glsl` about FIX 2, NEW-2, the
  one-unit-versus-two-units of headroom, and the hard-edge bug goes with them. That is roughly 60 lines of careful
  reasoning about a problem that stops existing.
- The GLSL twin of `radiusModulation` (`ssVortexF.glsl:73-82`) — **deleted**; the core function survives, CPU-only.
- `SSVortex::squashSegments`' funnel caller — **deleted** (bodies are point-placed; the squash sees one vertex quad
  each, like every puff).

Net: this change **removes more lines than it adds**, and retires a shader-to-core replication (`radiusModulation`'s
GLSL twin) rather than adding one — the pull operator's inverse is a new replication, so the count is a wash, but the
new one is in the replication set that already has the discipline and the review process around it.

### 3.2 What survives

- **The debris skirt.** `SSVortexDebrisSource` (`ssvortexrender.cpp:55-175`) is an `LLViewerPartSource` ticked by
  `LLViewerPartSim`. It never touched `ssVortexF.glsl` and is untouched by any of this. Dust at contact, the debris
  sheath, and the ground interaction all stay exactly where they are.
- **The whole of `ssvortexcore.h`'s taxonomy and lifecycle** — `childVortex`, `vortexAt`, phases, kinds, gates,
  `contactOffsetM`, `collarRadius`, `condensationShrink`, `collarAltitudeM`, `tiltOffsetM`, `baseAlpha`,
  `radiusModulation`, `wallCloudAltitudeM`, the dust devil lattice. Not one gate changes. `facingLight` goes with the
  shader that consumed it.
- **`SSVortices`** — the scheduler, the ranking, `MAX_ACTIVE`, `LiveVortex`, the collar table it resolves. The table is
  now consumed by `SSVolCloud::buildDeck` instead of by the render pass, which is the only wiring change.

### 3.3 Nothing needs a dedicated pass, and here is the argument that settles it

The temptation is to keep a small dedicated pass for the tightest near-contact detail — the condensation ring where the
funnel meets the ground, the dust collar, the debris sheath. Reject it, on the sorting argument that killed the
separate shaft pass (`far_clouds` §3, *Sorting*):

> shafts are emitted **into `mPuffs` itself** so the existing farthest-first painter's sort interleaves them correctly
> with puffs (a separate pass cannot — a 3 km shaft behind a 2 km puff would blend in the wrong order).

The funnel's case is *worse* than the shaft's, and the existing code already admits it. `ssvortexrender.cpp:517-527`
carries a "Review finding 10 (ordering residual, accepted)" comment stating that the funnel pass runs after
`SSVolCloud::render()` has submitted every puff, that a puff at the same depth range drawn earlier "can never be
occluded BY the funnel drawn after it in the alpha-blended sense ordinary back-to-front transparency needs", and that
this is accepted because "funnels are rare, small relative to the deck". That acceptance was reasonable for a ribbon.
It is not reasonable for a wall cloud: the funnel's top now sits **inside** the deck by construction, interleaved with
the very puffs the pull operator dragged around it, at the same depths, over hundreds of metres of overlap. A separate
pass cannot sort against that. The whole point of the change is that the funnel and the deck are the same fabric; a
second pass would re-introduce the seam the change exists to remove.

So: **no dedicated pass survives**. Near-contact detail is either (a) particles, which is what the debris skirt already
is and where a condensation ring belongs if it is ever wanted, or (b) more funnel bodies at a finer LOD level, which is
one increment of `L`. The pass is not the right tool for either.

---

## 4. The flag and the channel

**Get this right; a wrong claim here is found only at implementation time.** Here is the current state of every channel
the deck's vertex format carries, read out of the live source rather than remembered.

### 4.1 The vertex colour is full — all four channels, 8 bits each

`ssvolcloud.cpp:2600`:

```
gGL.color4f(puff.mForm, puff.mBuried,
            puff.mShaft ? (0.5f + 0.5f * puff.mDrive) : (puff.mPhase * 0.49f),
            puff.mAlpha);
```

| channel | occupant | range |
|---|---|---|
| `r` | `Puff::mForm` — the structural form term | [0,1] |
| `g` | `Puff::mBuried` — the buried depth the gloom grades over | [0,1] |
| `b` | **shaft**: `0.5 + 0.5 * mDrive` (8e item 4) — **puff**: `mPhase * 0.49` (the S1/S2 flow port) | [0, 0.49] ∪ (0.5, 1] |
| `a` | `Puff::mAlpha` — the edge fade | [0,1] |

The `b` channel is the one the prompt asked about, and it is **the same channel for both occupants**, partitioned at
0.5, not two channels. The fragment's top-level branch is literally `if (vary_color.b > 0.5)`
(`ssVolCloudF.glsl:1122`). The margin is tight and deliberate: the source comment computes it — `0.49 × 255 = 124.95`,
rounds to `125/255 = 0.4902` — because `LLRender`'s immediate-mode colour strider is `LLStrider<LLColor4U>`
(`llrender.h:527`), **8 bits per channel**. There are 125 usable phase codes below the threshold and 130 drive codes
above it. Repartitioning `b` a third way would cost both occupants precision they are already short of.

**Verdict: the colour is full. The funnel may not flag on `vary_color`.**

### 4.2 The texcoord payload is free — two channels, full float

`ssvolcloud.cpp:2620-2634` and `ssVolCloudV.glsl:166-175`:

```
// CPU:   texCoord2f(cornerMarker + px * 0.45, cornerMarker + py * 0.45)
// GLSL:  vec2 corner  = round(texcoord0);           // exact: pre-rasterizer, |payload| <= 0.45 < 0.5
//        vec2 payload = (texcoord0 - corner) / 0.45;
//        vary_texcoord0 = vec4(corner, payload);
```

The S1 flow-port widen turned `vary_texcoord0` from `vec2` to `vec4` and added a two-channel payload lane that costs
**zero interpolator slots** (varyings are `vec4`-granular). And then:

```
const F32 px = 0.f;
const F32 py = 0.f;
```

**Both payload channels are hard zero today, on every path.** `ssvolcloud.cpp:2621-2624` says so — *"payload is (0,0)
for every puff at this stage — nothing has anything to put there yet"* — and `ssVolCloudF.glsl:33` says the same from
the other side. There is exactly one write site (`ssvolcloud.cpp:2628-2634`), it is in the non-shaft branch, and it
writes zero. `twin_flowchannel.cpp` already pins the bit-identity of the zero case.

Three properties make this the right lane and not merely an available one:

1. **Full F32 precision.** `LLRender`'s texcoord strider is `LLStrider<LLVector2>` (`llrender.h:526`) — floats, not
   bytes. The funnel needs to carry `h01` and a body index or angle at better than 1/255.
2. **Decoded before interpolation.** The `round()`/subtract happens in the vertex stage on the untouched per-corner
   attribute, so the payload is exact per corner and constant across a card whose four corners agree. Interpolating it
   is a no-op, not an approximation.
3. **The card u/v survives alongside it.** `vary_texcoord0.xy` is the *rounded* corner (0 or 1), interpolated normally
   — a funnel body still gets a clean `[0,1]²` card coordinate for `shape`/`rim`/`ss_sphere_normal` **and** a payload.
   The shaft path deliberately writes no payload and reads `.xy` as raw card u/v; that stays true and unchanged.

**Verdict: the funnel flags on `vary_texcoord0.z`, and carries `h01` on `vary_texcoord0.w`.**

```
payload.z = 1.0  for a funnel body, 0.0 otherwise      (flag; the fragment tests > 0.5)
payload.w = h01  for a funnel body, 0.0 otherwise      (height fraction, 0 contact .. 1 wall cloud)
```

The funnel's slot index (which of the `MAX_ACTIVE` uniform slots holds its axis and ω) does **not** need a channel: the
fragment recovers it from the same `ss_vortex_axis[]` array by nearest-axis test, or — cheaper and exact — the flag
becomes `payload.z = (slot + 1)` with `slot ∈ [0, MAX_ACTIVE)`, so `z > 0.5` is still the flag and `int(z + 0.5) - 1`
is the slot. Take the second; it costs nothing and removes a search.

### 4.3 The branch order

```
if (vary_texcoord0.z > 0.5)      { funnel body }        // NEW, first
else if (vary_color.b > 0.5)     { virga shaft }        // unchanged
else                             { ordinary puff }      // unchanged
```

Because both existing paths write payload zero, inserting the branch is **bit-identical** for them, and provably so by
the same test that pinned the widen. That is a rung, not an assurance.

### 4.4 The funnel branch's body

It is the puff branch minus the column logic, plus one rotation:

- **keeps** `shape`/`rim` (the soft radial window and the hard stop), `ss_sphere_normal`, the triplanar cloud noise
  body, the whole shared lighting tail — wrap, sun facing, graze, gloom-over-buried, strike lighting, airlight, glow;
- **drops** the `n_map` column read and everything it drives — the tower window, the anvil, `floor_z`, the thick-base
  fill, the cap band. A funnel has no lid to consolidate into and no floor to cut flat, which is the same sentence
  `ssVolCloudF.glsl:1119` already writes about the shaft. **This is what keeps a funnel body from being carved away by
  the deck's own hole noise** — a funnel condenses where the *vortex* is, not where the presence field says a column
  grew, and the deck-presence question is answered once at emit time (§5, rung 5) rather than twice inconsistently;
- **adds** the noise-coordinate rotation of §2.2: rotate the triplanar sample position about `ss_vortex_axis[slot].xy`
  by `−ω·t` before the read, so the texture turns with the column.

Uniforms: two `vec4[MAX_ACTIVE]` arrays plus the count that already exists.

```
uniform int  ss_vortex_n;
uniform vec4 ss_vortex_axis[3];   // xy: axis at the deck base, z: reachM, w: omega (rad/s, signed)
uniform vec4 ss_vortex_pull[3];   // x: topZ, y: pullInMaxM, z: pullDownM, w: zFadeM
```

Six `vec4`. `ss_vortex_multi[]` is deleted along with the mask that read it.

---

## 5. The ladder

Every rung names its test and its **failing control** — the deliberately wrong implementation the rung must reject, so
that a green rung means something. Tests live in `V:\Scratch\atmo\tests`, never in the viewer tree.

**Rung 1 — the LOD law's body count against distance.** `unit_vortex_body_lod.cpp`. Sweeps distance 50 m → 8000 m at
each of 40 `(intensity, taper, condensation)` triples and asserts: `bodyCount` monotone non-increasing in distance;
`>= BODY_FLOOR_PER_FUNNEL` for every live funnel; `<= BODY_CAP_PER_FUNNEL` always; and — the real content — **the
nesting**: the body set at level `(L, A)` is a strict superset of the set at `(L−1, A)` and at `(L, A−1)`, compared as
sorted `(h01, θ)` pairs with `SS_CHECK_BITS`, so no surviving body moves when the level changes.
*Failing control:* an azimuth generator using `θ_j = 2π j / az` (the obvious even spacing) instead of the van der
Corput sequence — correct counts, correct spacing, and every body jumps at every level change. The rung must reject it.

**Rung 2 — the profile matches `ssvortexcore.h`'s existing table bit-for-bit.** `unit_vortex_body_profile.cpp`. For
every `h01 = i / 2^L` at every level, asserts the emitter's radius equals
`collarRadius(h01, intensity, taper, condensation) * radiusModulation(θ, t, ...)` with `SS_CHECK_BITS`, over the
`intensity × taper × condensation` grid the existing `collarRadius` tests already use, and asserts the emitter's
altitude equals `collarAltitudeM` and its axis offset equals `tiltOffsetM`.
*Failing control:* a "harmless" re-spelling of the taper — `base * pow(mix, taper)` in place of
`base * pow(mix, 1/taper)`, or the floor applied after `condensationShrink` instead of before it (which is precisely
the 48:1 step review 5b removed). Both produce a plausible funnel and both fail bit-equality. This rung exists because
the single largest risk in this whole change is an implementer writing a second radius profile that *looks* right.

**Rung 3 — the swirl's rotation rate against the core's own rotation.** `unit_vortex_swirl.cpp`. Asserts
`bodyOmegaRadS` has `sign == mRotSign` for every kind (`KIND_ANTICYCLONIC` negative, the rest following the parent);
is within the `[BODY_OMEGA_MIN_RAD_S, BODY_OMEGA_MAX_RAD_S]` rails; is monotone non-increasing in intensity at fixed
taper (bigger funnel, slower angle); and that a body's angle advances by exactly `ω · Δt` between two wall-clock
samples, driven at a simulated 30 fps and at 144 fps to the same wall time with `SS_CHECK_BITS` on the result
(determinism rung 1, frame-rate independence).
*Failing control:* an accumulated `θ += ω * dt` per frame instead of the closed-form `ω · t`. It looks identical on
one machine and diverges between the two frame rates — the lesson-13 failure, caught here rather than in a build.

**Rung 4 — the displacement operator: boundedness, injectivity and round trip.** `unit_deck_pull.cpp` +
`twin_deck_pull.cpp`. Asserts over a 200 000-sample grid spanning `r ∈ [0, 2·reach]`, `z ∈ [base − 600, base + 900]`,
`t ∈ [0, 10⁴ s]`: `|Δxy| ≤ 2·reach·(1 + PULL_IN_MAX_FRAC)`; `|Δz| ≤ PULL_DOWN_M`; `D ≡ 0` at and beyond `reachM` **with
zero derivative** (lesson 10); `∂r'/∂r > 0` and `∂z'/∂z > 0` everywhere (injectivity — the numeric margins §2.3 derives
are 0.33 and 0.825, and the rung asserts them as measured maxima); and the **round trip**: `invert(apply(p)) == p` to
`SS_CHECK_NEAR` 1e-4 m over the whole grid, plus the full composition round trip in 4·0's fixed order (hero → swirl →
pull → O(z), then the reverse) landing back on the producer's own cell. `twin_deck_pull.cpp` transliterates the GLSL
inverse and its **call site** — argument construction, sample point, enclosing branch (lesson 8) — and pins it against
the core over the same grid.
*Failing controls*, three of them, because this rung carries the most risk: (a) `PULL_IN_MAX_FRAC = 0.7`, which pushes
`g' = 1.05` past 1 and folds the radial map — the injectivity assertion must fire; (b) an observer that undoes the pull
*before* undoing O(z) instead of after — the composition round trip must fire, and this is the exact ordering mistake
lesson 12 was written about; (c) a `w_r` using a linear falloff instead of a smoothstep — bounded, invertible, and it
leaves a first-derivative discontinuity at the reach that the zero-derivative assertion must catch.

**Rung 5 — no funnel bodies where the deck has no puffs.** `scenario_vortex_no_deck.cpp`. Drives a full build at
coverage 0.0, 0.15 and 0.6 with a forced tornado (`mForcedKind = 2`) pinned at `age01 = 0.5` so the funnel is in
`PHASE_TOUCHDOWN`, and asserts: at coverage 0.0, **zero** puffs in `deck.mPuffs` carry the funnel flag; at 0.6, the
count is within the LOD law's prediction for the camera distance; and at 0.15, funnel bodies exist **only** at rows
whose location passes the same cell-occupancy verdict the fine tier used — read from the builder's own tally, not from
a fourth gate (the phase 6d discipline: no new gate, a tally of the existing gate's verdicts).
*Failing control:* the current renderer's own rule — emit whenever `vortexAt` reports a live funnel, using
`windProfileBaseZ()` as the top. It draws a full funnel at coverage 0.0. The rung must reject it, which is the same as
saying this rung is the regression test for the first screenshot.

**Rung 6 — the branch insertion is bit-identical for existing paths.** Extends `twin_flowchannel.cpp`. Asserts that
with `payload.z == 0` the shaft and puff branches produce byte-identical output to the pre-change shader over the
existing sample grid, and that `int(payload.z + 0.5) - 1` recovers the slot for every `slot ∈ [0, MAX_ACTIVE)` through
the F32 encode/round-trip.
*Failing control:* flagging on `vary_color.b` in a repartitioned band (`[0.33, 0.5)` for the funnel, phase squeezed to
`[0, 0.33)`) — it works, and it silently costs the phase channel 40 % of its 125 codes, which shows up as banding in
the advected detail. The rung asserts the phase decode's code count is unchanged.

**Rung 7 — visual.** `out/vortex_form_side.png` and `out/vortex_form_top.png`: the same funnel at 200 m, 800 m, 2 km
and 5 km, old ribbon beside new bodies, at three azimuths including directly overhead; and a top-down view of the deck
around the axis showing the pull operator's inward bulge with the reach circle drawn on it. Not an assertion — the
thing the user actually judges.

---

## 6. Cost, and the order of work

### 6.1 Cost

- **Puffs.** ≤ 96 bodies × ≤ 3 funnels = **288** on a default budget of 2520 (`SSAtmoCloudPuffBudget`), 11 %. In
  practice a single hero funnel at mid distance is 20–40 bodies. The builder cost is 288 × (`collarRadius` + one hash +
  one `SSDeckShade::puffForm`), negligible beside the 6400-cell walk that dominates `buildDeck`.
- **The budget trim, stated residual.** Bodies go into `deck.mPuffs` before the farthest-first sort
  (`ssvolcloud.cpp:1613`) and the budget erase-from-front (`:1617-1625`), like shafts. A near funnel is nowhere near
  the front and is never trimmed. A funnel beyond ~5 km under an at-budget overcast **can** be trimmed, and the failure
  mode is a distant funnel thinning rather than popping (the front of the vector is the far edge). Accepted for the
  first landing; the mitigation is the existing dial. Listed as an open question (§7·6) rather than solved with
  machinery.
- **Fragment.** Six `vec4` uniforms; the inverse-pull block is ~15 ALU in the shared frame block, early-outed on
  `ss_vortex_n == 0`, which is true on the overwhelming majority of frames on the overwhelming majority of skies. The
  funnel branch itself is the puff branch minus the `n_map` column read — **cheaper per fragment than an ordinary
  puff**.
- **Deleted.** 232 lines of shader, ~150 lines of draw loop, a shader program registration, one core function, one
  GLSL twin, and roughly 60 lines of comment reasoning about a padding problem that ceases to exist. Net line count is
  negative.

### 6.2 Where this sits, and the recommendation

It is genuinely entangled with 8b. The tornado is one of 8b's operators in 4·0's table; the wall cloud is listed as an
8b-3 emission part; and 8b-2's supercell swirl is the *same displacement operator* with different constants and a
different reach.

**Recommendation: land it first, as its own phase 8h, before 8b.** Three reasons, in order of weight:

1. **8b needs this machinery and this is the cheapest place to build it.** The displacement operator touches the full
   replication set — builder, `ssVolCloudF.glsl`, `bakeGroundShadow`, `precipNoiseAt` — and needs the producer/observer
   composition order, the inversion twin and the opus replication review. Building it once for a single small entity
   with a well-defined reach, then generalising it for the supercell swirl in 8b-2, is far cheaper and far safer than
   building it inside 8b's anatomy work, where the swirl arrives simultaneously with towers, anvils, mammatus, shelves
   and archetype selection. `ssdeckpullcore.h` is 8b-2's core, delivered early with a smaller blast radius.
2. **It is separable in every direction that matters.** It touches `ssvolcloud.cpp`/`.h` (an emit block and a Puff
   flag), the two deck shaders (one branch), `ssvortexrender.cpp` (deletion), `ssvortices.cpp` (hand the collar table
   to the builder), and it deletes `ssVortexV/F.glsl`. It does not touch archetype selection, part primitives, analytic
   carves, the anatomy LOD, or `ssanatomycore.h` — none of which exists yet. Nothing in 8b's sequence blocks it and it
   blocks nothing in 8b's sequence.
3. **It is what the user is looking at.** 8b is a multi-phase build (8b-1 through 8b-4); the funnel is the thing on
   screen in the current build, and it can be fixed inside one phase.

**One decision this forces, and it should be made now rather than discovered in 8b-3:** section 4's part list item 4
gives the archetype an emitted **wall cloud** ("a lowered block under the updraft, where the funnel hangs"). Section
4·0 gives the same wall cloud to the **displacement** operator ("the deck's own base bulges into the lowering and wall
cloud around the funnel"). They are the same object, specified twice, in two operator kinds. 4·0 is the later and
better decision, and this document takes it: **8b-3's emitted wall cloud is deleted.** The wall cloud is the pull
operator's output. 8b-3 keeps the shelf, the tail cloud and the flanking line, all of which are genuinely emission —
they have no deck fabric at their location to displace.

Revised order for section 7: `8d, 8e → 8a → **8h (this)** → 8b → 8c`.

---

## 7. Open questions

1. **Should a funnel be suppressed outright when there is no deck at its location, or should it draw a partial funnel
   from whatever fabric exists?** *Recommendation: suppressed, and by arithmetic rather than by a gate.* Bodies are
   emitted per row, each row asks the builder's own occupancy tally at its own location, and a row with no cloud emits
   nothing. A funnel under a broken deck therefore condenses in patches, top-down, which is what a forming funnel
   actually does. The vortex still exists in the simulation — the debris skirt and the dust still spin at contact —
   it simply has no condensation to show. This is the honest reading of "no cloud, no funnel" and it costs no new code
   path.

2. **The wall-cloud altitude comes from `windProfileBaseZ()` (the resolver) while the deck draws at `mBaseZ` (the live
   build), and they can disagree by a frame.** *Recommendation: keep `windProfileBaseZ()` for the simulation — it is
   the deterministic figure and SCHEDULER fix 4's reasoning still stands — but let the emitter snap `h01 = 1` to
   `deck.mBaseZ` at emit time.* Emission now happens inside `buildDeck`, which holds the current `mBaseZ`, so the
   funnel's top meets the deck's actual base exactly rather than approximately, with no determinism cost (the snap is a
   render-time placement, like the squash). If the user would rather the funnel's height be strictly a simulation
   quantity, say so and the snap goes.

3. **How dark is "a tad darker"?** *Recommendation: `mBuried = 1.0`, the D2 dark extreme, and nothing else — no colour,
   no new constant.* This is the smallest change that satisfies the clause and it rides a gradient the user can already
   see working on the deck's own bellies. If it reads as not dark enough on a build, the next move is `ss_gloom`'s
   curve — which affects the storm's whole underside coherently — and **not** a funnel-specific tint. Worth an explicit
   A/B on the first build, since it is free to judge and impossible to judge from here.

4. **Should each suction vortex become its own mini-column of bodies at close range, or is `radiusModulation` on the
   placement radius enough?** *Recommendation: `radiusModulation` only, for the first landing.* It gets the lumpy,
   breathing, multi-lobed silhouette for free from one existing formula, and a multi-vortex tornado seen close enough
   for individual suction vortices to resolve is rare enough that it should not drive the first design. Revisit after a
   build with `mMultiN >= 4` on screen.

5. **Waterspouts: a spray ring at the water contact?** *Recommendation: defer, and note it as the natural second user
   of the debris-skirt particle source rather than of anything in this document.* It is a contact effect, not a form
   change, and the form change should land alone.

6. **Should funnel bodies be exempt from the puff-budget trim?** *Recommendation: no, for the first landing.* Exempting
   them means partitioning the sorted vector before the erase, which is real machinery for a case (a >5 km funnel under
   an at-budget overcast) that thins rather than pops. If a build shows a hero funnel disappearing at range, the fix is
   a stable partition on the funnel flag, and the LOD law's `BODY_FLOOR_PER_FUNNEL` of 3 means the exempt set would be
   tiny.

7. **`SSVortex::MAX_ACTIVE` is 3, sized for a render budget of three collar stacks.** *Recommendation: leave it at 3 for
   this landing,* since the fragment's uniform slot array is now the binding constraint rather than the draw cost, and
   3 × 2 `vec4` is the right size for a term every deck fragment evaluates. But the constant's *reason* changes — from
   "three collar stacks at once" to "three displacement slots the fragment gate can afford" — and its comment must be
   rewritten to say so rather than left claiming a budget that no longer exists.
