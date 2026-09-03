# 2D UI system

## Widget framework (indra/llui)

Class chain: `LLView` (llview.h:98) → `LLUICtrl` (lluictrl.h:47) → `LLPanel` (llpanel.h:55) → `LLFloater` (llfloater.h:122, an LLInstanceTracker). `LLFloaterView` (llfloater.h:588) manages z-order/cascade/snap.

Drawing is an immediate-mode recursive tree walk, no retained scene graph. `LLView::drawChildren()` (llview.cpp:1293) iterates children **back-to-front**, culls against root rect + `LLView::sDirtyRect`, and per child does push/translate/draw/pop on a software matrix stack (`LLRender2D` via `LLUI::pushMatrix`, llui.h:323) feeding `gGL`. Primitives: `gl_rect_2d` (llrender2dutils.cpp:118), text via `LLFontGL::renderUTF8`.

## Where UI renders in the frame

`render_ui()` (llviewerdisplay.cpp:1687) runs after 3D: `render_ui_3d()` (:1913 — selection outlines, beacons) then `render_ui_2d()` (:1971). `setup2DRender()` (llviewerwindow.cpp:7008) sets the ortho projection. With `RenderUIBuffer` on, 2D UI draws into the `gPipeline.mUIScreen` FBO with dirty-rect scissoring (union'd with last frame's rect) and is blitted as a quad (llviewerdisplay.cpp:2025-2086); otherwise `gViewerWindow->draw()` draws direct. **The 2D UI is already effectively a compositable texture layer** — relevant to any external-UI plan.

Root hierarchy (llviewerwindow.cpp:2203+): `mRootView` → main view → `gFloaterView` (:2342), snapshot floater view, `gMenuHolder` (:2296), `gToolBarView`, `gConsole`, `gHUDView` (sent to back).

## XUI: XML → widgets

717 XML files in `skins/default/xui/en/` (277 floater_, 244 panel_, 157 menu_, plus notifications.xml, strings.xml). Flow: `LLUICtrlFactory::createFromFile<T>` (lluictrlfactory.h:152) → layered XML node (skin overlays) → per node: copy default `Params` → `LLXUIParser::readXUI` → `applyXUILayout` (topleft rect munging) → `validateBlock()` + `new T(params)` + `initFromParams` → recurse children → `postBuild()` (returning false DELETES the widget).

- Widget tag↔class binding is static registration per TU: `static LLDefaultChildRegistry::Register<LLButton> r("button");` (llbutton.cpp:59).
- Params system is `LLInitParam` (llcommon/llinitparam.h); XUI can round-trip back out via `writeXUI`.

## Floater lifecycle

`LLFloaterReg` is a static name→(xml, build func) map; all ~246 viewer registrations in `llviewerfloaterreg.cpp` (ss floaters at :692-705; `ss_atmo_audio` binds plain `LLFloater`, pure XML; `ss_atmo_fx` binds `SSFloaterEffects` for the one control XUI cannot bind, its cloud-field debug mask). `getInstance()` (llfloaterreg.cpp:180) finds-or-builds: build_func → `buildFromFile` → `applyControlsAndPosition` → `adjustToFitScreen`. Position/visibility persistence comes from `save_rect="true"` in XML → lazily declared rect/visibility controls (llfloaterreg.cpp:422-431, 511).

Typical wiring (see `ssfloateratmoenv.cpp:84+`): everything happens in `postBuild()` — `getChild<T>("name")->setCommitCallback(lambda)` directly onto model singletons. No event pumps in normal UI code.

## Input routing

`LLWindow` callbacks → `LLViewerWindow`. Mouse (`handleAnyMouseClick`, llviewerwindow.cpp:~1180): mouse captor (`gFocusMgr.getMouseCapture()`) first → `mRootView` recursion → current `LLToolMgr` tool → right-click pie menu fallthrough. Keyboard (`LLViewerWindow::handleKey`, :3372): keyboard focus → menu accelerators → floater tab nav → parent-chain walk → current tool → gesture triggers. `LLFocusMgr` (llfocusmgr.h:82) holds keyboard focus, mouse capture, and top ctrl (popups).

## UI↔logic plumbing

Predominantly direct C++ + boost-signal commit callbacks. XUI `function="..."` attributes resolve via `CommitCallbackRegistry` (lluictrl.h:300). Notifications: `LLNotifications` templated by notifications.xml. Inventory uses a clean model/observer split: `LLInventoryModel::notifyObservers` → `LLInventoryObserver::changed(U32 mask)` (llinventoryobserver.h:68) → `LLInventoryPanel::modelChanged` rebuilds `LLFolderView` items.

## External UI (Electron/Tauri) feasibility facts

- **LLLeap exists and works today** (llcommon/llleap.cpp, `--leap` option handled in llappviewer.cpp:1385-1406): spawns a child process, speaks **length-prefixed LLSD notation over stdin/stdout pipes**, bidirectional via named reply pumps. This is a ready-made external-process RPC bridge — pipes, not websockets. No websocket code exists outside llwebrtc (voice).
- **21 LLEventAPI listener classes** form a remote-control vocabulary: `LLUIListener` (invoke any registered UI callback by name, read widget values by XUI path), `LLFloaterRegListener` (show/hide/toggle floaters, clickButton), `LLNotificationsListener` (respond/cancel), `LLAgentListener`, `LLInventoryListener`, `LLWindowListener` (synthesize input), `LLViewerControlListener` (settings), etc.
- **Cleanly externalizable (pure 2D, model-backed)**: chat/IM, inventory, preferences, profiles, search, notifications, the ss floaters.
- **Entangled with 3D (keep native)**: pie menus (spawned from LLToolPie object picks), HUD attachments (drawn in llviewerdisplay.cpp:2007-2022), name tags/`llhud*`, the `lltool*` family (build/edit/drag-drop needs per-pixel picks and world raycasts), `LLToolDragAndDrop` (crosses inventory↔world).
- Practical path: Tauri/Electron child launched via LLLeap, a thin websocket↔LLEventPump shim inside the viewer (or speak LLLeap's stdio protocol directly), external UI subscribes to model observers (inventory `changed(mask)` masks serialize well) rather than reimplementing LLFolderView.

## Gotchas

- `postBuild()` returning false silently deletes the widget tree.
- Child lookup is by string name at runtime (`getChild<T>`) — typos compile fine and log-warn at runtime; a missing child returns a default-constructed dummy widget, not null.
- Draw order = reverse child order; front of child list is topmost.
- UI draws with `gUIProgram` and UI scale/zoom applied (llviewerwindow.cpp:3045-3070); anything drawn outside `setup2DRender` state will be misplaced or in the wrong space.
- Dirty-rect optimization (`LLView::sDirtyRect`) means a widget that animates without invalidating leaves stale pixels when `RenderUIBuffer` is on.
