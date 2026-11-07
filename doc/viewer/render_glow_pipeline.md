# The glow/bloom pipeline — verified trace

Traced end-to-end from source (pipeline.cpp renderFinalize/generateGlow, glowExtractF.glsl, postDeferredTonemap/GammaCorrect) because repeated glow bugs came from everyone reasoning about this from memory. What follows is what the code actually does; if it disagrees with intuition, the code wins.

## The chain

1. **`mRT->screen`** accumulates the frame after the deferred soften. Its ALPHA channel is written only by passes that draw into it with alpha writes on — the glowing-object machinery in the alpha pool, and the Atmo weather passes (lightning deliberately; puffs masked off; rain's alpha-blend contributes only a² ≈ nothing).
2. **renderFinalize**: `tonemap()` (HDR) or `gammaCorrect()` (non-HDR) copies screen → `mPostPingMap`. Both shaders end in `frag_color = diff` — **alpha passes through verbatim**, rgb is tonemapped and CLAMPED to 0–1.
3. **`generateGlow(&mPostPingMap)`** — note the source: the TONEMAPPED buffer, not the raw screen. glowExtract runs with `GLOW_MIN_LUMINANCE = 9999`, which makes the luminance and warmth terms `smoothstep(9999, 10000, …) = 0` for any real pixel. So the extract is `rgb = col.rgb (+dither)`, `a = col.a`, drawn into the cleared glow target with `BT_ADD_WITH_ALPHA`:

   **glow seed = tonemapped_rgb × screen_alpha. Nothing else. Ever.**

4. mGlow ping-pong blurs the seed; `combineGlow` composites it back over the frame.

## What this makes true (the part everyone gets wrong)

- **Brightness does not bloom.** The luminance path is deliberately dead (the 9999). An arbitrarily bright HDR disc, sun, or explosion generates ZERO bloom unless something wrote screen ALPHA at its pixels. Halos around the sun are the EEP sky's own authored glow term — sky COLOUR — not bloom.
- **G-buffer alpha can never bloom.** The sky pass, dome clouds and celestial discs write frag_data[0..3]; their alphas are art/blend/contribution weights consumed before soften. None of it reaches `mRT->screen.a`. (A celestial-disc "glow feed" dimming was once built on the opposite assumption; it was actually double-counting a blend weight.)
- **Blend mode shapes the alpha that does land** in the post-deferred screen: additive passes accumulate alpha linearly (lightning's bloom, by design), alpha-blended passes contribute only a² (rain never visibly blooms), masks contribute zero (the puff mask is load-bearing — their near-1 core alphas would bloom hard).
- **Glow is tonemap-clamped**: the seed rgb is 0–1, so bloom intensity is governed by alpha and the blur, not by HDR magnitude. A pass wanting stronger bloom writes MORE ALPHA, not brighter colour.

## Rules of engagement

Before touching any pass's alpha or blaming glow for an artifact: name the target it writes (G-buffer vs post-deferred screen vs own RT), name the blend mode, and derive what alpha means there. The historical antipattern — relocating passes around the pipeline until an artifact moved — traded side-effects without ever fixing the variable that mattered.
