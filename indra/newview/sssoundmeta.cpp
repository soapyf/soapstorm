/**
 * @file sssoundmeta.cpp
 * @brief See sssoundmeta.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "sssoundmeta.h"

#include "ssatmostore.h"

#include "ssatmomagic.h"
#include "ssprecippreset.h"

#include "llassetstorage.h"
#include "llaudiodecodemgr.h"
#include "llaudioengine.h"

// Stops and joins the worker threads at shutdown.
void SSSoundMeta::cleanupSingleton()
{
    {
        std::lock_guard<std::mutex> lock(mJobMutex);
        mStop = true;
    }
    mJobSignal.notify_all();
    for (std::thread& t : mWorkers)
    {
        if (t.joinable()) t.join();
    }
    mWorkers.clear();
}

// Lazily spins up the analysis worker pool, sized to leave a core free.
void SSSoundMeta::startWorkers()
{
    if (mStarted) return;
    mStarted = true;

    const S32 count = llclamp((S32)std::thread::hardware_concurrency() - 1, 1, 4);
    for (S32 i = 0; i < count; ++i)
    {
        mWorkers.emplace_back([this]()
        {
            for (;;)
            {
                Job job;
                {
                    std::unique_lock<std::mutex> lock(mJobMutex);
                    mJobSignal.wait(lock, [this]() { return mStop || !mJobs.empty(); });
                    if (mStop) return;
                    job = std::move(mJobs.front());
                    mJobs.pop_front();
                }

                Meta meta = analyze(job.mPCM, job.mChannels, job.mRate, job.mPurpose);
                meta.mLengthMS = job.mLengthMS;

                std::lock_guard<std::mutex> lock(mResultMutex);
                mResults.emplace_back(job.mID, meta);
            }
        });
    }
}

// The whole offline analysis of one PCM clip: envelope, onset/peak/tail, level, impact cadence (with gap repair), gap floor, crackiness.
SSSoundMeta::Meta SSSoundMeta::analyze(const std::vector<S16>& pcm, S32 channels, F32 rate, U32 purpose)
{
    Meta meta;
    if (pcm.empty() || channels < 1 || rate <= 0.f) return meta;

    const U32 frames = (U32)(pcm.size() / (size_t)channels);
    const U32 window = llmax((U32)(rate * 0.010f), 1u);
    const U32 count = frames / window;
    if (count < 2) return meta;

    std::vector<F32> envelope(count, 0.f);
    F32 peak = 0.f;
    U32 peak_at = 0;
    for (U32 w = 0; w < count; ++w)
    {
        F64 sum = 0.0;
        const S16* p = pcm.data() + (size_t)w * window * channels;
        for (U32 i = 0; i < window * (U32)channels; ++i)
        {
            const F64 v = (F64)p[i] / 32768.0;
            sum += v * v;
        }
        const F32 rms = (F32)sqrt(sum / (F64)(window * channels));
        envelope[w] = rms;
        if (rms > peak) { peak = rms; peak_at = w; }
    }
    if (peak <= 0.0001f) return meta;

    meta.mPeakMS = (U32)((F32)(peak_at * window) * 1000.f / rate);

    if (purpose & PURPOSE_DENSITY)
    {
        F64 sum_env = 0.0;
        for (U32 w = 0; w < count; ++w) sum_env += envelope[w];
        meta.mDensity = (F32)(sum_env / (F64)count) / peak;
    }

    {
        const U32 points = 240;
        meta.mEnvelope.assign(points, 0.f);
        for (U32 w = 0; w < count; ++w)
        {
            const U32 x = (U32)((U64)w * points / count);
            meta.mEnvelope[x] = llmax(meta.mEnvelope[x], envelope[w] / peak);
        }
    }

    U32 onset = peak_at;
    while (onset > 0 && envelope[onset - 1] >= peak * 0.2f) --onset;
    meta.mOnsetMS = (U32)((F32)(onset * window) * 1000.f / rate);

    U32 tail = count - 1;
    while (tail > 0 && envelope[tail] < peak * 0.05f) --tail;
    meta.mTailMS = (U32)((F32)((tail + 1) * window) * 1000.f / rate);

    {
        const S32 half = llmax((S32)(0.5f * rate / (F32)window), 1);
        const S32 lo = llmax((S32)peak_at - half, 0);
        const S32 hi = llmin((S32)peak_at + half, (S32)count - 1);
        F32 sum_env = 0.f;
        for (S32 w = lo; w <= hi; ++w) sum_env += envelope[(size_t)w];
        meta.mPeakLevel = sum_env / (F32)(hi - lo + 1);
    }

    if (purpose & PURPOSE_STEPS)
    {
        S32 impacts = 0;
        const U32 refractory = llmax((U32)(0.280f * rate / (F32)window), 1u);
        U32 last = 0;
        bool armed = true;
        for (U32 w = 1; w < count; ++w)
        {
            if (armed && envelope[w] > peak * 0.35f && envelope[w] > envelope[w - 1])
            {
                ++impacts;
                last = w;
                armed = false;
                if (meta.mOnsets.size() < 128)
                {
                    meta.mOnsets.push_back((U32)((F32)(w * window) * 1000.f / rate));
                }
            }
            else if (!armed && w - last > refractory && envelope[w] < peak * 0.2f)
            {
                armed = true;
            }
        }
        const F32 seconds = (F32)frames / rate;
        meta.mImpactRate = (seconds > 0.1f) ? (F32)impacts / seconds : 0.f;

        if (meta.mOnsets.size() >= 4)
        {
            F64 t_est = 0.0;
            for (size_t k = 0; k + 1 < meta.mOnsets.size(); ++k)
            {
                const F64 iv = (F64)(meta.mOnsets[k + 1] - meta.mOnsets[k]);
                if (iv >= 280.0 && (t_est == 0.0 || iv < t_est)) t_est = iv;
            }
            if (t_est >= 280.0 && t_est <= 1300.0)
            {
                F64 sum = 0.0; S32 n_ref = 0;
                for (size_t k = 0; k + 1 < meta.mOnsets.size(); ++k)
                {
                    const F64 iv = (F64)(meta.mOnsets[k + 1] - meta.mOnsets[k]);
                    const F64 mult = llmax(1.0, (F64)llround(iv / t_est));
                    if (fabs(iv / mult - t_est) < t_est * 0.3) { sum += iv / mult; ++n_ref; }
                }
                if (n_ref >= 2) t_est = sum / (F64)n_ref;

                std::vector<U32> repaired;
                for (size_t k = 0; k + 1 < meta.mOnsets.size(); ++k)
                {
                    repaired.push_back(meta.mOnsets[k]);
                    const F64 iv = (F64)(meta.mOnsets[k + 1] - meta.mOnsets[k]);
                    const S32 mult = (S32)llround(iv / t_est);
                    if (mult >= 2 && fabs(iv / (F64)mult - t_est) < t_est * 0.25)
                    {
                        for (S32 m = 1; m < mult; ++m)
                        {
                            repaired.push_back(meta.mOnsets[k] + (U32)(iv * (F64)m / (F64)mult));
                            ++meta.mRepaired;
                        }
                    }
                }
                repaired.push_back(meta.mOnsets.back());
                if (meta.mRepaired > 0)
                {
                    meta.mOnsets.swap(repaired);
                    const F32 seconds = (F32)frames / rate;
                    if (seconds > 0.1f) meta.mImpactRate = (F32)meta.mOnsets.size() / seconds;
                }
            }
        }

        if (meta.mOnsets.size() >= 4)
        {
            F64 sum = 0.0, sum2 = 0.0;
            const size_t n = meta.mOnsets.size() - 1;
            for (size_t k = 0; k < n; ++k)
            {
                const F64 iv = (F64)(meta.mOnsets[k + 1] - meta.mOnsets[k]);
                sum += iv;
                sum2 += iv * iv;
            }
            const F64 mean = sum / (F64)n;
            if (mean > 1.0)
            {
                const F64 var = llmax(sum2 / (F64)n - mean * mean, 0.0);
                meta.mCadenceCV = (F32)(sqrt(var) / mean);
            }
        }

        if (meta.mOnsets.size() >= 3)
        {
            F64 sum_gap = 0.0;
            U32 n_gap = 0;
            for (size_t k = 0; k + 1 < meta.mOnsets.size(); ++k)
            {
                const U32 a = meta.mOnsets[k];
                const U32 b = meta.mOnsets[k + 1];
                if (b <= a + 250) continue;
                const U32 lo_ms = a + (U32)((b - a) * 0.55f);
                const U32 hi_ms = b - 80;
                if (hi_ms <= lo_ms) continue;
                const U32 lo_w = (U32)((F32)lo_ms * rate / 1000.f) / window;
                const U32 hi_w = llmin((U32)((F32)hi_ms * rate / 1000.f) / window, count - 1);
                for (U32 w = lo_w; w <= hi_w; ++w) { sum_gap += envelope[w]; ++n_gap; }
            }
            if (n_gap > 0) meta.mGapFloor = (F32)(sum_gap / (F64)n_gap) / peak;
        }
    }

    if (purpose & PURPOSE_BRIGHT)
    {
        const U32 lo_f = peak_at * window;
        const U32 hi_f = llmin(lo_f + (U32)rate, frames - 1);
        S32 crossings = 0;
        S16 prev = pcm[(size_t)lo_f * channels];
        for (U32 f = lo_f + 1; f <= hi_f; ++f)
        {
            const S16 cur = pcm[(size_t)f * channels];
            if ((prev < 0) != (cur < 0)) ++crossings;
            prev = cur;
        }
        const F32 span_s = (F32)(hi_f - lo_f) / rate;
        meta.mCrackiness = (span_s > 0.01f)
            ? llclamp(((F32)crossings / span_s) / 8000.f, 0.f, 1.f) : 0.f;
    }

    return meta;
}

// Ready metadata for a sound, or null while it is still pending.
const SSSoundMeta::Meta* SSSoundMeta::get(const LLUUID& id)
{
    auto it = mEntries.find(id);
    return (it != mEntries.end() && it->second.mState == READY) ? &it->second.mMeta : nullptr;
}

// How many sounds have finished analysis, for status UI.
S32 SSSoundMeta::readyCount()
{
    S32 n = 0;
    for (const auto& e : mEntries) if (e.second.mState == READY) ++n;
    return n;
}

// How many sounds are queued or in flight, for status UI.
S32 SSSoundMeta::pendingCount()
{
    S32 n = 0;
    for (const auto& e : mEntries) if (e.second.mState == PENDING || e.second.mState == ANALYZING) ++n;
    return n;
}

// Registers a CSV of sound ids for analysis, unioning purposes and remembering who configured them.
// Every call records a slot for the debug view, including ones that name no valid sound.
void SSSoundMeta::addList(const std::string& csv, const std::string& source, U32 purpose)
{
    if (purpose == 0) return;

    SlotInfo& slot = mSlots.emplace_back();
    slot.mSource = source;
    slot.mPurpose = purpose;

    std::vector<std::string> tokens;
    LLStringUtil::getTokens(csv, tokens, ",+");
    for (const std::string& tok : tokens)
    {
        LLUUID id(tok);
        if (id.isNull()) continue;
        slot.mSounds.push_back(id);
        auto it = mEntries.emplace(id, Entry()).first;
        if (it->second.mSource.empty()) it->second.mSource = source;
        it->second.mPurpose |= purpose;
    }
}

// Walks every configured sound source - thunder, global footsteps, the active preset's ambiences and steps - into the entry table.
void SSSoundMeta::gather()
{
    mSlots.clear();

    for (const std::string& key : { SSAtmoStoreKey::THUNDER_CRACK, SSAtmoStoreKey::THUNDER_RUMBLE })
    {
        addList(SSAtmoStore::getString(key), key,
                PURPOSE_TIMING | PURPOSE_LEVEL | PURPOSE_BRIGHT);
    }

    for (S32 sf = 0; sf < STEP_SURFACE_COUNT; ++sf)
    {
        for (S32 ac = 0; ac < STEP_ACTION_COUNT; ++ac)
        {
            if (SSFootstepSounds::surfaceIsGlobal((SSStepSurface)sf))
            {
                const std::string name = SSFootstepSounds::globalSettingName((SSStepSurface)sf, (SSStepAction)ac);
                addList(SSAtmoStore::getString(name), name, PURPOSE_STEPS);
            }
        }
    }

    const SSPrecipPreset& preset = SSPrecipPresetManager::instance().active();
    const char* ambience_names[] = { "ambient_light", "ambient_medium", "ambient_heavy", "roof_open", "roof_small", "roof_medium", "roof_big" };
    const std::string* ambiences[] = { &preset.mSounds.mAmbientLight, &preset.mSounds.mAmbientMedium,
                                       &preset.mSounds.mAmbientHeavy, &preset.mSounds.mRoofOpen,
                                       &preset.mSounds.mRoofSmall, &preset.mSounds.mRoofMedium,
                                       &preset.mSounds.mRoofBig };
    for (S32 b = 0; b < 7; ++b)
    {
        addList(*ambiences[b], "preset:" + preset.mName + "/" + ambience_names[b], PURPOSE_DENSITY);
    }
    for (S32 sf = 0; sf < STEP_SURFACE_COUNT; ++sf)
    {
        for (S32 ac = 0; ac < STEP_ACTION_COUNT; ++ac)
        {
            addList(preset.mFootsteps.mSounds[sf][ac],
                    std::string("preset:") + preset.mName + "/step_" + SSFootstepSounds::surfaceKey((SSStepSurface)sf) + "_" + SSFootstepSounds::actionKey((SSStepAction)ac),
                    PURPOSE_STEPS);
        }
    }
}

// Fetches a sound from the asset server when the cache lacks it, then queues the decode; a no-op for sounds already decoded, failed, or marked corrupt.
void SSSoundMeta::fetch(const LLUUID& id)
{
    if (id.isNull() || !gAudiop || !gAssetStorage) return;
    if (gAudiop->isCorruptSound(id)) return;

    LLAudioData* data = gAudiop->getAudioData(id);
    if (!data || data->hasDecodedData() || data->hasDecodeFailed()) return;

    if (data->hasLocalData())
    {
        LLAudioDecodeMgr::getInstance()->addDecodeRequest(id);
        return;
    }

    // Atmo's ambience goes to the front of the asset queue - these are the viewer's own sounds, not scenery.
    if (!mFetching.insert(id).second) return;
    gAssetStorage->getAssetData(id, LLAssetType::AT_SOUND, onAssetFetched, NULL, true);
}

// Asset-server reply for fetch(): the raw sound is now in the asset cache, so mark it local and queue the decode that writes the .dsf; failures are flagged on the data so the pump reports them instead of waiting out the timeout.
void SSSoundMeta::onAssetFetched(const LLUUID& id, LLAssetType::EType, void*, S32 status, LLExtStat)
{
    if (SSSoundMeta::instanceExists()) SSSoundMeta::getInstance()->mFetching.erase(id);
    if (!gAudiop) return;

    LLAudioData* data = gAudiop->getAudioData(id);
    if (!data) return;

    if (status != 0)
    {
        LL_WARNS("SSSoundMeta") << "sound asset fetch failed: " << id << "  "
            << LLAssetStorage::getErrorString(status) << " (" << status << ")" << LL_ENDL;
        data->setHasDecodeFailed(true);
        data->setHasLocalData(false);
        data->setHasDecodedData(false);
        data->setHasCompletedDecode(true);
        return;
    }

    data->setHasDecodeFailed(false);
    data->setHasLocalData(true);
    LLAudioDecodeMgr::getInstance()->addDecodeRequest(id);
}

// Fetches every configured sound that is not yet decoded, feeds decoded PCM to the workers a few at a time, and fails entries whose fetch or decode broke or never finished.
void SSSoundMeta::pump()
{
    if (!gAudiop) return;

    {
        std::lock_guard<std::mutex> lock(mJobMutex);
        if (mJobs.size() >= 4) return;
    }

    const F64 now = SSAtmoMagic::getInstance()->sharedTime();
    S32 queued = 0;
    for (auto& pair : mEntries)
    {
        Entry& entry = pair.second;
        if (entry.mState != PENDING) continue;

        LLAudioData* data = gAudiop->getAudioData(pair.first);
        if (!data)
        {
            entry.mState = FAILED;
            entry.mFailWhy = "no audio data";
            continue;
        }

        if (entry.mFirstTried < 0.0) entry.mFirstTried = now;

        if (!data->hasDecodedData())
        {
            if (gAudiop->isCorruptSound(pair.first))
            {
                entry.mState = FAILED;
                entry.mFailWhy = "marked corrupt by the audio engine";
                LL_WARNS("SSSoundMeta") << "sound marked corrupt: " << pair.first
                    << "  configured in [" << entry.mSource << "]" << LL_ENDL;
                continue;
            }
            if (data->hasDecodeFailed())
            {
                entry.mState = FAILED;
                entry.mFailWhy = "asset fetch or decode failed";
                LL_WARNS("SSSoundMeta") << "sound fetch or decode failed: " << pair.first
                    << "  configured in [" << entry.mSource << "]" << LL_ENDL;
                continue;
            }
            if (now - entry.mFirstTried > 60.0)
            {
                entry.mState = FAILED;
                entry.mFailWhy = "never decoded in 60s";
                LL_WARNS("SSSoundMeta") << "sound never decoded: " << pair.first
                    << "  configured in [" << entry.mSource << "]" << LL_ENDL;
                continue;
            }
            // Every configured sound is requested up front; only the analysis jobs below are capped.
            if (!entry.mFetchIssued)
            {
                entry.mFetchIssued = true;
                fetch(pair.first);
            }
            continue;
        }

        if (queued >= 3) continue;

        LLAudioBuffer* buffer = data->getBuffer();
        if (!buffer)
        {
            gAudiop->updateBufferForData(data, pair.first);
            buffer = data->getBuffer();
        }
        if (!buffer)
        {
            if (data->hasWAVLoadFailed())
            {
                entry.mState = FAILED;
                entry.mFailWhy = "decoded file would not load";
            }
            continue;
        }

        Job job;
        job.mID = pair.first;
        job.mLengthMS = buffer->getLengthMS();
        job.mPurpose = entry.mPurpose;
        if (!buffer->getPCMCopy(job.mPCM, job.mChannels, job.mRate))
        {
            entry.mState = FAILED;
            entry.mFailWhy = "no PCM copy";
            continue;
        }

        entry.mState = ANALYZING;
        {
            std::lock_guard<std::mutex> lock(mJobMutex);
            mJobs.push_back(std::move(job));
        }
        mJobSignal.notify_one();
        ++queued;
    }
}

// Per-frame drive: gather occasionally, pump decodes, and publish finished results to the table.
void SSSoundMeta::idle()
{
    if (!SSAtmoMagic::getInstance()->isSwitchedOn()) return;

    startWorkers();

    const F64 now = SSAtmoMagic::getInstance()->sharedTime();
    if (mLastGather < 0.0 || now - mLastGather > 5.0)
    {
        mLastGather = now;
        gather();
    }

    pump();

    std::vector<std::pair<LLUUID, Meta>> done;
    {
        std::lock_guard<std::mutex> lock(mResultMutex);
        done.swap(mResults);
    }
    for (auto& pair : done)
    {
        Entry& entry = mEntries[pair.first];
        entry.mMeta = pair.second;
        entry.mState = READY;

        LL_INFOS("SSSoundMeta") << pair.first << "  len " << pair.second.mLengthMS
            << "ms  onset " << pair.second.mOnsetMS << "ms  tail " << pair.second.mTailMS
            << "ms  level " << pair.second.mPeakLevel << "  impacts/s " << pair.second.mImpactRate
            << "  density " << pair.second.mDensity << "  gapfloor " << pair.second.mGapFloor << "  cv " << pair.second.mCadenceCV << "  repaired " << pair.second.mRepaired
            << "  crackiness " << pair.second.mCrackiness << LL_ENDL;
    }
}
