# Alpha-mask verdicts and texture discard levels

## The symptom

A mesh with a texture that is almost entirely opaque, but carries an alpha channel
with a few soft or cut-out texels, renders alpha-blended while the texture is still
fetching and snaps to correct once the full-resolution level arrives. In the blended
window the face writes no depth and is distance-sorted against every other alpha face
in its spatial group, so it shows the classic sorting artefacts: parts of the object
drawing behind things they are in front of, and self-overlap flicker as the camera
moves.

This is stock behaviour, not a regression. It has nothing to do with Squeeze; toggling
BC7 serving does not change it.

## Why it happens

`LLImageGL::analyzeAlpha` (`indra/llrender/llimagegl.cpp`) decides whether a texture
"qualifies for masking". It runs on **every** upload, at whatever discard level the
upload is, and it overwrites `mIsMask` each time. `LLFace::canRenderAsMask` reads that
flag when the face's draw info is generated, and the answer picks between the
deferred alpha-mask pass (depth write, no sorting) and the forward blended pass.

The heuristic is a histogram of quantised alpha. Two things make its "not a mask"
answer unreliable on a coarse mip:

- decimation averages hard cut-out edges into mid-range alpha values, so a 1-texel
  edge that is well under the 1/48 midrange threshold at full resolution can be a
  large fraction of a 32x32 level;
- the function deliberately adds a 2x2 box-averaged copy of every sample to the
  histogram in order to "mid-skew" it, which was written to catch high-frequency
  masks that alias badly, and it compounds the first effect.

A third rule, "all samples close to opaque but not all totally opaque", was written for
intentionally near-invisible textures. On a coarse mip of a mostly-solid texture it
fires for the same reason: the averaged soft texels stop being exactly 255.

So the verdict at discard 5 is often "blend" and the verdict at discard 0 is "mask".
Stock does not rebuild the faces when the verdict changes; the fix-up you see at full
resolution is a side effect of some other rebuild (mesh LOD, texture size change).

## What the change does

Two small pieces.

**Provisional verdicts on coarse mips.** `analyzeAlpha` keeps the stock rules intact,
then, only when they said "not a mask" **and** the upload is coarser than
`LLImageGL::sSSAlphaMaskTrustedDiscard`, treats the texture as a mask if all but 1/48
of the samples sit in the top histogram bucket (alpha 240..255). Anything a coarse mip
says that is not near-uniform opacity is left alone, so foliage, fences and other real
cut-outs behave exactly as before at every level. Once a trusted discard (2 or finer by
default) uploads, its verdict stands as stock computed it.

**Rebuild on a flip.** `LLViewerFetchedTexture::ssSyncAlphaMaskVerdict` remembers the
verdict the faces were last built against and dirties them when a later upload
changes it. It runs from `postCreateTexture` (the first main-thread point after the
upload, whichever thread did the GL work) and after a BC7 upload from the Squeeze
store. This is what makes the provisional guess safe: if a finer level overturns it in
either direction, the faces move to the right pass on the next rebuild instead of
waiting for an unrelated one.

## Setting

`SSAlphaMaskTrustedDiscard` (S32, default 2). `-1` restores stock behaviour, every
level is trusted. Changing it affects the next upload of each texture; resident
textures keep their verdict until they re-upload.

## Cost

One extra `dirtyTexture()` sweep per texture per verdict flip, which happens at most a
couple of times over a texture's fetch and is coalesced with the component-change
rebuild the first arrival already triggers. No per-frame cost.
