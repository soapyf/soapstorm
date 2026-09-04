# HUD supersampling

Antialiasing for HUD attachment geometry, controlled by `SSHUDSupersample` (1 = off, 2 or 4 = supersample factor).

## Why HUDs have no antialiasing

`render_hud_attachments()` in `indra/newview/llviewerdisplay.cpp` runs from `display()` *after* `gPipeline.renderFinalize()`. FXAA and SMAA both live inside `renderFinalize`, ahead of the final present blit, so by the time HUD geometry draws the post chain has finished and resolved to the default framebuffer. The pipeline has no MSAA either — `RenderFSAAType` selects none, FXAA or SMAA, all post-process. HUD attachments therefore land in a single-sample buffer with no antialiasing of any kind.

## Why supersampling rather than post AA

Running FXAA or SMAA over the HUD would only address polygon silhouettes, and would do that badly: both detect edges from luma, and a HUD render is mostly transparent, so there is no meaningful background to find edges against.

Most visible HUD jaggedness is not on polygon edges at all. It is on the alpha edges inside the texture — a transparent PNG magnified across a large screen region. Supersampling antialiases those too, because it supersamples the shading, including the texture fetch. HUD geometry is trivial and low-overdraw, so the fill cost is affordable in a way it would not be for the world.

## How it works

1. **Point-upscale the presented frame** into a target sized world-view × factor, with a `GL_NEAREST` `glBlitFramebuffer` from the default framebuffer.
2. **Clear depth only** and render the HUD geometry pass into that target, completely unmodified.
3. **Box-resolve** each factor × factor block back over the screen with blending off, replacing the frame, and emit `gl_FragDepth` from the same block.

The depth half of step 3 is not optional. HUD attachments write depth, and things drawn after them — avatar nametags, non-HUD hover text, look-at indicators, beacons — depth test against it to stay hidden behind the HUD. Rendering into an offscreen target strands that depth in `mHUDScreen`, so the resolve hands it back. Depth cannot be averaged like colour, so the resolve takes the nearest sample in each block — a partially covered edge pixel occludes, erring towards the HUD hiding what is behind it. Crucially, the resolve also samples world depth from `mRT->deferredScreen` (`worldDepthMap`) and resolves `min(nearest, world_depth)`, so that where no HUD attachment is present (or where world geometry is nearer), the world depth buffer is preserved rather than overwritten with the far plane; this ensures that look-at indicators, nametags, and other 3D debug overlays remain correctly occluded by objects and terrain in the distance.

The upscale step is what keeps this simple. The HUD blends against a real background exactly as it does when drawn straight to the screen, so nothing has to be done about blend modes, colour masks, or what the alpha channel means — no draw pool, shader or `LLRender` behaviour changes at all.

The background survives the round trip untouched. Every one of a destination pixel's factor × factor source texels is the same point-sampled texel, so their unweighted average is that texel back again. Only the HUD geometry, which is genuinely rasterised at the higher resolution, gains anything.

### Why not composite with an alpha channel instead

The obvious alternative — render the HUD alone into a transparent target and alpha-composite the resolve — does not work without substantial surgery, because the pipeline never produces usable coverage:

- `renderGeomPostDeferred` sets `gGL.setColorMask(true, false)`, so alpha writes are masked off for the whole pass.
- Where alpha *is* written the pipeline treats it as **glow**, not coverage: `LLDrawPoolAlpha` uses `(BF_ZERO, BF_ONE_MINUS_SOURCE_ALPHA)` for glow suppression, and its emissive pass switches to `(BF_ONE, BF_ONE)` to accumulate glow additively.
- HUD passes that draw with `GL_BLEND` off write their shader's alpha verbatim, and `pbropaqueF.glsl` emits a hard `0.0`.

An earlier version of this feature overrode all of that centrally in `LLRender`. It produced a target whose alpha was still zero in practice, which turned the premultiplied composite into pure addition and hazed the entire frame. The upscale approach sidesteps the question rather than fighting it.

## Known limitations

- **The resolve averages in display space.** HUDs render after `renderFinalize` has gamma corrected, so the target holds sRGB-ish values and the box filter averages those rather than linear light. Slightly incorrect, and consistent with how FXAA already behaves here.
- **Scope is the geometry pass only.** `render_hud_elements()` stays outside the target — it draws text and selection overlays sized in screen pixels, which would come out at half scale in a target this size.
- **Bandwidth.** Unlike an alpha-composited HUD-only target, this reads and rewrites the whole world view each frame on top of the HUD fill cost.
- **Memory.** The target is world-view sized × factor, RGBA plus depth: roughly 33 MB each at 1080p and 2×. The factor steps down automatically when it would exceed `mGLMaxTextureSize`, and the default is 2.

## Where the pieces are

| Piece | Location |
| --- | --- |
| Setting | `SSHUDSupersample` in `indra/newview/app_settings/settings.xml` |
| Target, upscale, resolve | `LLPipeline::beginHUDSupersample` / `endHUDSupersample`, `indra/newview/pipeline.cpp` |
| Call site | `render_hud_attachments()`, `indra/newview/llviewerdisplay.cpp` |
| Resolve shader | `indra/newview/app_settings/shaders/class1/deferred/hudDownsampleF.glsl` |
| FBO accessor | `LLRenderTarget::getFBO()`, `indra/llrender/llrendertarget.h` |
