# Combat render lockout

Debug rendering exists for building and diagnosing, and most of it either draws through
geometry or reveals things the scene deliberately hides. In a Second Life Military
Community (SLMC) fight that is a wallhack. Wireframe alone turns every building into a
cage with the players visible inside. This document records what the lockout covers, how
it decides when to engage, and why each toggle is handled the way it is.

## When it engages

`SSCombatLockout::update()` runs once per frame from `display()` before any render pass.
It samples `LLAgentCamera::cameraMouselook()`, the same accessor the mouselook combat
features (IFF, crosshair tint, hit marker) already use, so mouselook and over-the-shoulder
(OTS) are treated identically. The lockout holds for five seconds after the camera leaves
mouselook. Flicking out to check a debug view and back in gains nothing, and five seconds
is short enough that authoring work in third person is not slowed down.

The lockout is client-side and unconditional. It does not depend on parcel, region, group,
or any RLV relay. An earlier idea of a sim-pushed "combat zone" flag was dropped: the
trigger is the act of aiming, which needs no server cooperation and cannot be spoofed by
the client's own settings.

## What it covers

Two mechanisms, chosen per toggle by what the code around it needs.

**Read-gated.** The draw site checks `SSCombatLockout::active()` (or the debug mask is
filtered in `LLPipeline::hasRenderDebugMask`). The user's toggle is untouched and simply
has no effect while locked. Used where nothing downstream caches the state:

- Render Metadata bits: bounding boxes, octree, occlusion, raycast, collision skeleton,
  joints, agent target, physics shapes, impostor extents.
- Atmo Magic overlays that are geometry-derived maps: wind flow, rain shadow, roof runoff,
  geometry settling, surface field, world field. Plus the rain column trace setting.
- Beacons floater: all beacon kinds and highlights, gated at the single dispatch block.
- Avatar hitboxes (`DebugRenderHitboxes`). Aim convergence targets this same box, so it
  is an aiming aid, not just a reveal.
- Show Look At and Show Point At: an unseen opponent's aim direction.
- Footstep audio markers: depth compare is off on the label, so it tracks every walking
  avatar through walls. Existing marks are killed on engage, not just new ones refused.

**Forced with a stash.** The live flag has to be flipped because something else caches
it or the pipeline manipulates it mid-frame. The user's intent is stashed on engage and
restored on release; the menu handlers route through the stash while locked so a toggle
made in mouselook takes effect when the lockout lifts rather than being lost:

- Wireframe. Eight read sites plus occlusion setup key off `gUseWireframe`.
- Highlight Transparent and Highlight Rigged Transparent. Invisible faces only build
  render batches while highlighted, so the flip needs `rebuildDrawInfo()` either way.
- Show Hidden Selection. Selection silhouettes drawn through occluders.
- Rendering Types that hold world geometry: simple, materials, alpha, alpha mask,
  fullbright, fullbright alpha mask, tree, terrain, water, void water, volume, grass,
  bump, PBR. Read-gating `hasRenderType` is wrong here because the pipeline pushes and
  clears the type mask between passes, so a forced-on type would leak into a pass that
  deliberately excluded it. Avatars, particles, sky, clouds and glow stay user-controlled:
  turning those off only hurts the player doing it.

## IFF markers

The mouselook IFF marker is a screen-space marker with no depth test, drawn for every
avatar in range. Line of sight now gates the marker and the crosshair identification
alike, always, in mouselook. The old preference that let the line-of-sight check be
turned off for identification was removed; an anti-cheat rule cannot be a user setting.
Avatars whose active group matches the agent's active group are allies and keep their
markers and identification through walls. The two-ray check (avatar centre, then head
height) is re-cast per target at most ten times a second and the last answer reused
between casts, which keeps a crowded region cheap.

## Not covered, on purpose

- Fog rendering feature toggle. It is a fixed-function fossil: the only reader is the
  legacy atmospherics colour update, which feeds beacon tinting and a small sky
  reflection blend. Distance haze lives in the shaders and never reads it.
- Cloud field overlay and the Atmo info readout: sky and weather telemetry only.
- Attached lights and particles: turning them off is defensive.
- Area search, object facts and region floaters: text listings, not rendering.
