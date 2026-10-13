# Long Sound Upload (SS:Nexii)

Sounds longer than the grid clip limit (30s SL / 60s OpenSim) are split into parts and uploaded as `name 01`, `name 02`, … instead of being rejected. Part count is `ceil(length / limit)` so the L$ cost is minimal either way. By default each part is cut at max length with a shorter final part; Preferences > Soapstorm > Sounds has a checkbox (`FSLongSoundUploadEqualSplits`) to cut near-equal parts instead, for when a stubby tail is awkward to sequence.

Every encoded sound (split or not) gets a 10ms fade-in/out applied during vorbis encoding (`LLVORBIS_CLIP_FADE_TIME`). This kills clicks at clip edges generally, and because split segments run through the same encoder, the cut points are de-clicked for free — the splitter itself is a plain PCM copy.

## Loudness normalization (LUFS)

`sslufs.cpp` implements ITU-R BS.1770-4 gated integrated loudness (K-weighting biquads with de-quantized coefficients so any sample rate works; 400ms blocks at 75% overlap; -70 LUFS absolute and -10 LU relative gates; clips under 400ms measure as a single block).

**Downloaded sounds** (`FSNormalizeDownloadedSounds`, default off; target `FSSoundTargetLUFS`, default -23 like EBU R128 / the common games standard): fresh asset decodes are normalized on the General thread pool inside the decode worker, so the sound cache `.dsf` and the in-memory buffer are born normalized. Sounds already cached from before play immediately as-is while a background worker measures the cache file; if it is off-target by more than 0.5 LU the file is rewritten and the live sound is in-flight patched: `LLAudioEngine::reloadSoundBuffer` swaps the buffer under the existing `LLAudioSource` (so loop, trigger vs. attached, position, gain, and sync semantics are preserved untouched) and resumes each playing channel at its captured PCM byte position via FMOD `get/setPosition`. Viewer UI sounds (`UISnd*`) are exempt from all download processing. Each asset is checked at most once per session.

## Test plan (loudness)

- Enable downloaded-sound normalization, play an old loud cached sound: it starts at original volume, then within a moment hot-swaps to normalized loudness *at the same playback position*; a looping sound keeps looping, an attached sound stays attached.
- Set the target slider to a different LUFS: spinner and slider stay in sync.
- UI sounds (clicks, alerts) are unaffected.
- Disable the option: no cache rewrites occur.

## Test plan

- Upload a valid wav under 30s via Build > Upload > Sound: behaves as before, but the encoded ogg has faded edges (no click on loop/end).
- Upload a wav over 30s via Upload > Sound: a confirmation dialog reports part count and total L$ cost; accepting uploads `name 01..NN`, each ≤ 30s, cuts click-free when played back-to-back.
- Bulk upload (Upload > Bulk) with a long wav in the mix: cost estimate counts each part; upload produces the numbered parts without any encode failure.
- Invalid wavs (wrong rate, non-PCM, truncated chunk) still show their original error notifications.
- On OpenSim grids the limit is 60s; splitting kicks in only past that.
- Toggle the equal-splits checkbox in Preferences > Soapstorm > Sounds: a 70s upload gives 30s + 30s + 10s parts unchecked, ~23.3s x3 checked.
