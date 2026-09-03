# Glow vs alpha — the definitive rules

The #1 recurring bug source: **framebuffer alpha means different things per stage**. Companion: `doc/render_glow_pipeline.md` (verified glow trace). All paths under `indra/newview`.

## The one invariant

**From the moment `mRT->screen` is bound (deferred soften) until tonemap, its alpha channel is NOT coverage — it is the bloom mask.** Coverage alpha exists only inside a fragment shader as a blend factor; it must never survive into destination alpha unless you want bloom.

Proof: `pipeline.cpp:8192` forces `GLOW_MIN_LUMINANCE = 9999`, zeroing the luminance/warmth terms in `glowExtractF.glsl:48-49`, collapsing the extract to `frag_color.a = max(col.a, 0)`. **The bright-pass is pure alpha readback — brightness never blooms, only alpha does.**

## Glow data path

1. Per-face `te->getGlow()` → vertex EMISSIVE attribute, packed as `LLColor4U(0,0,0,glow)` — glow rides the attribute's **alpha** (llface.cpp:2266).
2. Face joins `PASS_GLOW`/`PASS_GLTF_GLOW` only if `!is_alpha && sRenderGlow && glow>0` (llvovolume.cpp:7248).
3. `LLDrawPoolGlow::renderPostDeferred`: `setColorMask(false,true)` + `BT_ADD` into `screen`; `emissiveF.glsl:35` writes `vec4(0,0,0, diffuse.a*vertex_color.a)` — alpha only. GLTF: `pbrglowF.glsl:64` writes `a = max(emissive.rgb) * vertex_emissive.a`.
4. Alpha pool's emissive sub-pass: `blendFunc(ZERO, ONE, ONE, ONE)` — rgb untouched, alpha purely additive (lldrawpoolalpha.cpp:860).
5. Tonemap/gammaCorrect/CAS pass alpha through verbatim into `mPostPingMap`.
6. `generateGlow`: extract with `BT_ADD_WITH_ALPHA` (= `SRC_ALPHA, ONE`) ⇒ **glow seed = tonemapped rgb × screen alpha** (pipeline.cpp:8183-8226) → gaussian ping-pong (`RenderGlowIterations*2` passes, 8-tap, ×glowStrength) → `combineGlow` pure add (glowcombineF.glsl:37).
7. **Side effect**: `mGlow[1]` also feeds auto-exposure luminance (pipeline.cpp:7913, luminanceF.glsl:63) — stray glow darkens the whole scene via exposure.

## What alpha means, per target

| Target/pass | `.a` means |
|---|---|
| G-buffer att0 (albedo) | legacy **emissive/fullbright mix** (consumed at softenLightF.glsl:270); PBR/simple write 0. NOT coverage — use discard/alpha-mask for cutouts (alphaF.glsl:236 "without breaking glow"). |
| G-buffer att1 (spec) | legacy glossiness exponent; PBR 0 |
| G-buffer att2 (normal) | gbuffer FLAG (float codes, ±0.1 compare) |
| G-buffer att3 (emissive RT) | unused (emissive is RGB); sky/clouds/celestial write blend weights here — safe, consumed before soften, **can never bloom** |
| `screen` after soften | 0 — softenLightF.glsl:284 explicitly wipes glow for opaque geometry; RT pre-cleared to a=0 ("zeroing alpha (glow) is important", pipeline.cpp:9765) |
| `screen` during post-deferred | **glow accumulation** |
| haze passes | src alpha = transmittance, used to attenuate both dst rgb AND dst glow: `blendFunc(ONE, SRC_ALPHA, ZERO, SRC_ALPHA)` |
| water (class3 waterF.glsl:376) | `spec * mask` — deliberate: sun glint feeds bloom |
| post-chain after combineGlow | junk (FXAA variant overwrites alpha with luma) |
| impostors | real coverage mask (pipeline.cpp:12291) |

## Blend/colormask conventions

- **Default post-deferred state: `setColorMask(true,false)` — alpha writes OFF.** You must opt IN to touching glow.
- Forward alpha pool: `setColorMask(true,true)` with `blendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ZERO, ONE_MINUS_SRC_ALPHA)` — "glow suppression" (lldrawpoolalpha.cpp:239-255): an alpha-blended object **erases glow behind it** proportionally and contributes none.
- Glow passes: `setColorMask(false,true)` + `BT_ADD`.
- Blend mode shapes what alpha lands: additive passes accumulate alpha linearly (lightning bloom, by design); alpha-blended contributes only a²≈nothing (rain); masked contributes zero.

## Rules of engagement (write these into any rendering change)

1. **Before touching any pass, name three things**: which target it writes (G-buffer / post-deferred screen / own RT), the blend func, and the colormask. Then derive what alpha means there. Never relocate a pass to chase an artifact.
2. **Post-deferred pass wanting transparency**: `setColorMask(true,false)`, let `frag_color.a` be coverage (blend still reads SRC_ALPHA). Precedents: ssvolcloud.cpp:558, sspreciprenderer.cpp:696, sslightningrender.cpp:502.
3. **Post-deferred pass wanting bloom**: `setColorMask(true,true)`, treat `frag_color.a` as glow STRENGTH sized independently of opacity (lightning uses `glow*a*0.08`). More bloom = more alpha, not brighter rgb — glow seed is tonemap-clamped.
4. **Writing alpha=1 in post-deferred screen** ⇒ full-strength halo bleeding well past silhouettes (low-res blur) + scene-wide exposure drop. alpha=0 ⇒ nothing. Linear in between.
5. **G-buffer shaders**: never put coverage in `frag_data[0].a` — it's the fullbright mix factor.
6. HDR rgb can't bloom (extract is alpha-only) but unclamped color still blows out the tonemapper — clamp body color (ssvolcloudF.glsl:460 documents the "halo around every puff" fix).

## Historical breakages (don't repeat)

- Debug overlays writing 0.7-0.9 alpha → "every line bloomed into an unreadable neon smear".
- Cloud puffs alpha-blending with alpha writes on → corner halos; fixed with `setColorMask(true,false)`.
- A celestial-disc "glow feed" built on the assumption G-buffer alpha blooms — it was double-counting a blend weight; G-buffer alpha can never reach screen alpha.
- Precip: emissive material intentionally uses `(SRC_ALPHA, ONE, SRC_ALPHA, ONE)` (sspreciprenderer.cpp:582); the lit/decal materials beside it use the glow-suppressing pool blend — copy the right one.
