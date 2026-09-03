# Networking, messages, login

## UDP message system (indra/llmessage)

`LLMessageSystem` (message.h:279) owns the UDP socket, `LLPacketRing`, `LLCircuit` table, and a template reader + LLSD reader (swapped per-scope by the `LockMessageChecker` RAII, message.cpp:4001). `message_template.msg` (~235K) is parsed at startup; per-message flavor overrides in `app_settings/message.xml`.

- **Handlers are flat C function pointers**, ~123 registered in `register_viewer_callbacks()` (llstartup.cpp:3815). No queue — handlers run **inline on the main thread inside decode**.
- Receive path `checkMessages()` (message.cpp:491): packet ring → strip appended acks → `zeroCodeExpand` → circuit/reliable/dup handling → template decode → handler.
- **Decode is the hot spot**: `LLTemplateMessageReader::decodeData` (lltemplatemessagereader.cpp:536) heap-allocates an `LLMsgData` per message and a `LLMsgBlkData` **per block repetition**, then copies every field's bytes; the handler re-reads via `getBinaryDataFast` into yet another buffer. A 200-object update = hundreds of allocations + full payload copies, all main-thread.
- Per-frame pump: `LLAppViewer::idleNetwork()` (llappviewer.cpp:6713), adaptive budget starting at `CheckMessagesMaxTime` 20ms (grows 3.5%/frame under backlog), plus `MESSAGE_MAX_PER_FRAME` cap; then `processAcks` (resends, watchdogs). Throttles: `LLViewerThrottle` sends `AgentThrottle` (7 channels).

## HTTP stack

`llcorehttp` = threaded libcurl wrapper: requests/replies cross via queues to the `_httpservice` worker; main thread drains via `HttpRequest::update()`. Policy classes (llappcorehttp.cpp:70-112): AP_DEFAULT, AP_TEXTURE, AP_MESH1/2, AP_LONG_POLL, AP_INVENTORY, with per-class connection limits. Coroutine layer: `HttpCoroutineAdapter::postAndSuspend/getAndSuspend`; `LLCoprocedureManager` named pools (default 5 coros, big queue).

**Capabilities**: region seed cap POST of `buildCapabilityNames` list (llviewerregion.cpp:259), retried; `setCapability` per entry.

**Event poll**: creating the `EventQueueGet` cap constructs `LLEventPoll` (llviewerregion.cpp:3648); `eventPollCoro` (lleventpoll.cpp:158) long-polls on AP_LONG_POLL. Curl timeout/499/502-504 = normal "no events". Backoff `1s + 3s*errors`; >15 errors on the **agent's region** → `forceDisconnect`. Events are unpacked on the coro thread then **posted per-message to the "mainloop" WorkQueue** (lleventpoll.cpp:190, :384-407 — an LLSD shallow-copy flagged in-comment as a crash risk), dispatched via the `LLHTTPNode` tree at `/message/<name>`. Arrives via event poll (not UDP): `EstablishAgentCommunication`, `CoarseLocationUpdate`, all ChatterBox/IM, group data updates, display names, `ObjectPhysicsProperties`, voice info.

## Login (llstartup.cpp state machine)

XMLRPC login in `viewer_components/login/lllogin.cpp` (`loginCoro`), dispatched by `LLLoginInstance` (TOS/MFA/update failure handling). `STATE_*` progression (llstartup.h:58):

FIRST → FETCH_GRID_INFO → AUDIO_INIT → BROWSER_INIT → LOGIN_SHOW → LOGIN_WAIT → LOGIN_CLEANUP → AGENTS_WAIT → LOGIN_AUTH_INIT (connect :1793) → LOGIN_PROCESS_RESPONSE → WORLD_INIT → MULTIMEDIA_INIT (circuit opened: `UseCircuitCode` reliable, :2489) → SEED_GRANTED_WAIT → SEED_CAP_GRANTED → WORLD_WAIT (spins on `gGotUseCircuitCodeAck`) → AGENT_SEND (`send_complete_agent_movement`) → AGENT_WAIT (waits `AgentMovementComplete`; timeout → `reset_login()`) → INVENTORY_SEND/SKEL (skeleton from XMLRPC response) → MISC → PRECACHE → WEARABLES_WAIT → CLEANUP → STARTED.

`RegionHandshake` → `unpackRegionHandshake` → `RegionHandshakeReply`. Startup has its own message pumps (`do_startup_frame` budget 2ms/100 msgs).

## Disconnect / relogin

Detection, three paths: (1) circuit watchdog pings (`LLCircuit::updateWatchDogTimers`, llcircuit.cpp:801); (2) `idleNetwork` tail compares agent region `isAlive()` frame-over-frame → `forceDisconnect` (llappviewer.cpp:6799); (3) event-poll error exhaustion.

- `forceDisconnect` (llappviewer.cpp:5673): logout request + `YouHaveBeenLoggedOut` notification. `badNetworkHandler` routes mangled packets here + forces cache purge.
- `disconnectViewer()` (llappviewer.cpp:6814) is the **quit** path: caches inventory, saves name cache, `LLWorld::resetClass()`, `LLVOCache::deleteSingleton()`, sets `gDisconnected` latch (one-shot).
- **Relogin without restart exists**: `reset_login()` (llstartup.cpp:4242) tears down agent/world/appearance/voice and rewinds to STATE_BROWSER_INIT → login screen. It overlaps `disconnectViewer` cleanup — known fragility if extending either.
- Teleport: `process_teleport_finish` sends `UseCircuitCode` to the new sim + `CompleteAgentMovement`; region crossing via `process_crossed_region` (new seed cap). `EstablishAgentCommunication` pre-opens neighbor circuits; `DisableSimulator` tears down + `killObjects(region)`.

## Object updates

`ObjectUpdate`/`Compressed`/`Cached` → `LLViewerObjectList::processObjectUpdate` (llviewerobjectlist.cpp:514). Per object: a **second** full copy into a stack 2KB buffer (:581); non-temporary compressed full updates divert into the region VO cache and skip (:619); terse updates resolve UUID from (local_id, ip, port). Cache misses batch into `RequestMultipleObjects` (`requestCacheMisses`, llviewerregion.cpp:3149). Interest list mode (default/360) per region via `setInterestListMode`. `killObject` only marks dead; `cleanDeadObjects` compacts with a 10ms/frame budget.

## Gotchas / perf notes

- The ENTIRE UDP path (read, zero-decode, template decode with per-block allocs, handler, acks) is synchronous on the main thread inside `idleNetwork()`. Off-thread work is only curl + coroutine pools, and both marshal results back to main.
- Message handler ordering assumptions: some messages arrive via event poll, some UDP — don't assume a channel.
- Event-poll LLSD copy crash risk (lleventpoll.cpp:392 comment) when touching that code.
- `reset_login()` vs `disconnectViewer()` overlap — audit both when changing teardown.
