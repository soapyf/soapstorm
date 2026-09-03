/**
 * @file audioengine.h
 * @brief Definition of LLAudioEngine base class abstracting the audio support
 *
 * $LicenseInfo:firstyear=2000&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */


#ifndef LL_AUDIOENGINE_H
#define LL_AUDIOENGINE_H

#include <list>
#include <map>
#include <array>
#include <set> // <SS:Nexii> loudness normalization bookkeeping

#include "v3math.h"
#include "v3dmath.h"
#include "lltimer.h"
#include "lluuid.h"
#include "llframetimer.h"
#include "llassettype.h"
#include "llextendedstatus.h"
// <FS:minerjr> [FIRE-36022] - Removing my USB headset crashes entire viewer
// Need to include for audio device mutex shared with other audio/voice systems.
#include "inlinemutexs.h"
// </FS:minerjr> [FIRE-36022]

#include "lllistener.h"

#include <boost/signals2.hpp> // <FS:Ansariel> Output device selection

const F32 LL_WIND_UPDATE_INTERVAL = 0.1f;
const F32 LL_WIND_UNDERWATER_CENTER_FREQ = 20.f;

const F32 ATTACHED_OBJECT_TIMEOUT = 5.0f;
const F32 DEFAULT_MIN_DISTANCE = 2.0f;

#define LL_MAX_AUDIO_CHANNELS 30
// <FS:Ansariel> FIRE-4276: Increase number of audio buffers
//#define LL_MAX_AUDIO_BUFFERS 40   // Some extra for preloading, maybe?
#define LL_MAX_AUDIO_BUFFERS 60
// </FS:Ansariel>

class LLAudioSource;
class LLAudioData;
class LLAudioChannel;
class LLAudioChannelOpenAL;
class LLAudioBuffer;
class LLStreamingAudioInterface;
struct SoundData;

//
//  LLAudioEngine definition
//
class LLAudioEngine
{
    friend class LLAudioChannelOpenAL; // bleh. channel needs some listener methods.

public:
    enum LLAudioType
    {
        AUDIO_TYPE_NONE    = 0,
        AUDIO_TYPE_SFX     = 1,
        AUDIO_TYPE_UI      = 2,
        AUDIO_TYPE_AMBIENT = 3,
        AUDIO_TYPE_COUNT   = 4 // last
    };

    enum LLAudioPlayState
    {
        // isInternetStreamPlaying() returns an *S32*, with
        // 0 = stopped, 1 = playing, 2 = paused.
        AUDIO_STOPPED = 0,
        AUDIO_PLAYING = 1,
        AUDIO_PAUSED = 2
    };

    LLAudioEngine();
    virtual ~LLAudioEngine();

    // initialization/startup/shutdown
    virtual bool init(void *userdata, const std::string &app_title);
    virtual std::string getDriverName(bool verbose) = 0;
    virtual LLStreamingAudioInterface *createDefaultStreamingAudioImpl() const = 0;
    virtual void shutdown();

    // Used by the mechanics of the engine
    //virtual void processQueue(const LLUUID &sound_guid);
    virtual void setListener(LLVector3 pos,LLVector3 vel,LLVector3 up,LLVector3 at);
    virtual void updateWind(LLVector3 direction, F32 camera_height_above_water) = 0;
    virtual void idle();
    virtual void updateChannels();

    //
    // "End user" functionality
    //
    virtual bool isWindEnabled();
    virtual void enableWind(bool state_b);

    // Use these for temporarily muting the audio system.
    // Does not change buffers, initialization, etc. but
    // stops playing new sounds.
    void setMuted(bool muted);
    bool getMuted() const { return mMuted; }
#ifdef USE_PLUGIN_MEDIA
    LLPluginClassMedia* initializeMedia(const std::string& media_type);
#endif
    F32 getMasterGain();
    void setMasterGain(F32 gain);

    F32 getSecondaryGain(S32 type);
    void setSecondaryGain(S32 type, F32 gain);

    F32 getInternetStreamGain();

    virtual void setDopplerFactor(F32 factor);
    virtual F32 getDopplerFactor();
    virtual void setRolloffFactor(F32 factor);
    virtual F32 getRolloffFactor();
    virtual void setMaxWindGain(F32 gain);


    // Methods actually related to setting up and removing sounds
    // Owner ID is the owner of the object making the request
    // NaCl - Sound Explorer
    void triggerSound(const LLUUID &sound_id, const LLUUID& owner_id, const F32 gain,
                      const S32 type = LLAudioEngine::AUDIO_TYPE_NONE,
                      const LLVector3d &pos_global = LLVector3d::zero,
                      // <FS:Testy> Optional parameter for setting the audio source UUID
                      // const LLUUID& source_object = LLUUID::null);
                      const LLUUID& source_object = LLUUID::null,
                      const LLUUID& audio_source_id = LLUUID::null);

    // NaCl End
    void triggerSound(SoundData& soundData);

    bool preloadSound(const LLUUID &id);

    void addAudioSource(LLAudioSource *asp);
    void cleanupAudioSource(LLAudioSource *asp);

    LLAudioSource *findAudioSource(const LLUUID &source_id);
    LLAudioData *getAudioData(const LLUUID &audio_uuid);

    // Internet stream implementation manipulation
    LLStreamingAudioInterface *getStreamingAudioImpl();
    void setStreamingAudioImpl(LLStreamingAudioInterface *impl);
    // Internet stream methods - these will call down into the *mStreamingAudioImpl if it exists
    void startInternetStream(const std::string& url);
    void stopInternetStream();
    void pauseInternetStream(S32 pause);
    void updateInternetStream(); // expected to be called often
    LLAudioPlayState isInternetStreamPlaying();
    // use a value from 0.0 to 1.0, inclusive
    void setInternetStreamGain(F32 vol);
    std::string getInternetStreamURL();

    // For debugging usage
    virtual LLVector3 getListenerPos();

    LLAudioBuffer *getFreeBuffer(); // Get a free buffer, or flush an existing one if you have to.
    LLAudioChannel *getFreeChannel(const F32 priority); // Get a free channel or flush an existing one if your priority is higher
    void cleanupBuffer(LLAudioBuffer *bufferp);

    bool hasDecodedFile(const LLUUID &uuid);
    bool hasLocalFile(const LLUUID &uuid);

    bool updateBufferForData(LLAudioData *adp, const LLUUID &audio_uuid = LLUUID::null);


    // <SS:Nexii> downloaded-sound loudness normalization; values pushed in from viewer settings
    void setSoundNormalization(bool enabled, F32 target_lufs)          { mNormalizeSounds = enabled; mNormalizeTargetLUFS = target_lufs; }
    void setSoundNormalizationExempt(const uuid_vec_t& ids)            { mNormalizeExempt.clear(); mNormalizeExempt.insert(ids.begin(), ids.end()); }
    bool isSoundNormalizationEnabled() const                           { return mNormalizeSounds; }
    F32  getSoundNormalizationTarget() const                           { return mNormalizeTargetLUFS; }
    bool isSoundNormalizationExempt(const LLUUID& id) const            { return mNormalizeExempt.find(id) != mNormalizeExempt.end(); }
    void markLoudnessChecked(const LLUUID& id)                         { mLoudnessChecked.insert(id); }
    void scheduleLoudnessCheck(const LLUUID& id); // background-check a cached decoded sound, then in-flight patch it
    void reloadSoundBuffer(const LLUUID& id);     // swap the loaded buffer for the rewritten cache file, preserving playback state
    // </SS:Nexii>

    // <FS:Ansariel> Asset blacklisting
    void removeAudioData(const LLUUID& audio_uuid);

    // Asset callback when we're retrieved a sound from the asset server.
    void startNextTransfer();
    static void assetCallback(const LLUUID &uuid, LLAssetType::EType type, void *user_data, S32 result_code, LLExtStat ext_status);

    // <FS:Ansariel> Output device selection
    typedef std::map<LLUUID, std::string> output_device_map_t;
    virtual output_device_map_t getDevices();
    virtual void setDevice(const LLUUID& device_uuid) { };

    typedef boost::signals2::signal<void(output_device_map_t output_device_map)> output_device_list_changed_callback_t;
    boost::signals2::connection setOutputDeviceListChangedCallback(const output_device_list_changed_callback_t::slot_type& cb)
    {
        return mOutputDeviceListChangedCallback.connect(cb);
    }

    void OnOutputDeviceListChanged(output_device_map_t output_device_map);
    // </FS:Ansariel>

    friend class LLPipeline; // For debugging
public:
    F32 mMaxWindGain; // Hack.  Public to set before fade in?

protected:
    virtual LLAudioBuffer *createBuffer() = 0;
    virtual LLAudioChannel *createChannel() = 0;

    virtual bool initWind() = 0;
    virtual void cleanupWind() = 0;
    virtual void setInternalGain(F32 gain) = 0;

    void commitDeferredChanges();

    virtual void allocateListener() = 0;


    // listener methods
    virtual void setListenerPos(LLVector3 vec);
    virtual void setListenerVelocity(LLVector3 vec);
    virtual void orientListener(LLVector3 up, LLVector3 at);
    virtual void translateListener(LLVector3 vec);


    F64 mapWindVecToGain(LLVector3 wind_vec);
    F64 mapWindVecToPitch(LLVector3 wind_vec);
    F64 mapWindVecToPan(LLVector3 wind_vec);

protected:
    LLListener *mListenerp;

    bool mMuted;
    void* mUserData;

    S32 mLastStatus;

    bool mEnableWind;

    LLUUID mCurrentTransfer; // Audio file currently being transferred by the system
    LLFrameTimer mCurrentTransferTimer;

    // A list of all audio sources that are known to the viewer at this time.
    // This is most likely a superset of the ones that we actually have audio
    // data for, or are playing back.
    typedef std::map<LLUUID, LLAudioSource *> source_map;
    typedef std::map<LLUUID, LLAudioData *> data_map;

    source_map mAllSources;
    data_map mAllData;

    std::array<LLAudioChannel*, LL_MAX_AUDIO_CHANNELS> mChannels;

    // Buffers needs to change into a different data structure, as the number of buffers
    // that we have active should be limited by RAM usage, not count.
    std::array<LLAudioBuffer*, LL_MAX_AUDIO_BUFFERS> mBuffers;

    F32 mMasterGain;
    F32 mInternalGain;          // Actual gain set; either mMasterGain or 0 when mMuted is true.
    F32 mSecondaryGain[AUDIO_TYPE_COUNT];

    F32 mNextWindUpdate;

    LLFrameTimer mWindUpdateTimer;

    // <FS:Ansariel> Output device selection
    output_device_list_changed_callback_t mOutputDeviceListChangedCallback;

    // <SS:Nexii> downloaded-sound loudness normalization state
    bool mNormalizeSounds;
    F32 mNormalizeTargetLUFS;
    std::set<LLUUID> mNormalizeExempt;   // viewer UI sounds etc, never processed
    std::set<LLUUID> mLoudnessChecked;   // checked or normalized this session
    // </SS:Nexii>

private:
    void setDefaults();
    LLStreamingAudioInterface *mStreamingAudioImpl;

    // <FS:ND> Protect against corrupted sounds

    std::map<LLUUID,U32> mCorruptData;

public:
    void markSoundCorrupt( LLUUID const & );
    bool isCorruptSound( LLUUID const& ) const;

    // </FS:ND>
};




//
// Standard audio source.  Can be derived from for special sources, such as those attached to objects.
//


class LLAudioSource
{
public:
    // owner_id is the id of the agent responsible for making this sound
    // play, for example, the owner of the object currently playing it
    // NaCl - Sound Explorer
    LLAudioSource(const LLUUID &id, const LLUUID& owner_id, const F32 gain, const S32 type = LLAudioEngine::AUDIO_TYPE_NONE, const LLUUID& source_id = LLUUID::null, const bool isTrigger = true);
    // NaCl End
    virtual ~LLAudioSource();

    virtual void update();                      // Update this audio source
    void updatePriority();

    void preload(const LLUUID &audio_id); // Only used for preloading UI sounds, now.

    void addAudioData(LLAudioData *adp, bool set_current = true);

    void setForcedPriority(const bool ambient)                      { mForcedPriority = ambient; }
    bool isForcedPriority() const                                   { return mForcedPriority; }

    void setLoop(const bool loop)                           { mLoop = loop; }
    bool isLoop() const                                     { return mLoop; }

    void setSyncMaster(const bool master)                   { mSyncMaster = master; }
    bool isSyncMaster() const                               { return mSyncMaster; }

    void setSyncSlave(const bool slave)                     { mSyncSlave = slave; }
    bool isSyncSlave() const                                { return mSyncSlave; }

    void setQueueSounds(const bool queue)                   { mQueueSounds = queue; }
    bool isQueueSounds() const                              { return mQueueSounds; }

    void setPlayedOnce(const bool played_once)              { mPlayedOnce = played_once; }

    void setType(S32 type)                                  { mType = type; }
    S32 getType()                                           { return mType; }

    void setPositionGlobal(const LLVector3d &position_global)       { mPositionGlobal = position_global; }
    // <SS:Nexii> For sources that follow the listener (thunder placed on a bearing): matching the listener's velocity zeroes the relative motion FMOD's doppler works from, so walking does not pitch-bend a clap.
    void setVelocity(const LLVector3 &velocity)                     { mVelocity = velocity; }

    // Where playback should BEGIN inside the asset, ms; consumed by the channel when it starts. Lets a footstep loop open on a random step instead of the recording's first one forever, and is
    // the primitive future chained/windowed playback rides on.
    void setStartOffsetMS(U32 ms)                                   { mStartOffsetMS = ms; }
    U32 getStartOffsetMS() const                                    { return mStartOffsetMS; }
    LLVector3d getPositionGlobal() const                            { return mPositionGlobal; }
    LLVector3 getVelocity() const                                   { return mVelocity; }
    F32 getPriority() const                                         { return mPriority; }

    // Gain should always be clamped between 0 and 1.
    F32 getGain() const                                             { return mGain; }
    virtual void setGain(const F32 gain)                            { mGain = llclamp(gain, 0.f, 1.f); }

    // <SS:Nexii> How buried this source is behind solid build, 0 clear to 1 fully enclosed. Applied by the backend as a LOW-PASS, not a volume cut: transmission loss through mass rises steeply with frequency (~6dB/octave), so occluded sound goes muffled and bassy long before it goes quiet - the neighbour's party is a bassline. Callers keep gain for loudness; this is for timbre.
    void setOcclusion(F32 occ)                                      { mOcclusion = llclamp(occ, 0.f, 1.f); }
    F32 getOcclusion() const                                        { return mOcclusion; }

    const LLUUID &getID() const     { return mID; }
    // NaCl - Sound Explorer
    const LLUUID &getLogID() const { return mLogID; }
    // NaCl End
    bool isDone() const;
    bool isMuted() const { return mSourceMuted; }

    LLAudioData *getCurrentData();
    LLAudioData *getQueuedData();
    LLAudioBuffer *getCurrentBuffer();

    bool setupChannel();

    // Stop the audio source, reset audio id even if muted
    void stop();

    // Start the audio source playing,
    // takes mute into account to preserve previous id if nessesary
    bool play(const LLUUID &audio_id);

    bool hasPendingPreloads() const;    // Has preloads that haven't been done yet

    friend class LLAudioEngine;
    friend class LLAudioChannel;
protected:
    void setChannel(LLAudioChannel *channelp);
    LLAudioChannel *getChannel() const                      { return mChannelp; }
    // NaCl - Sound Explorer
    static void logSoundPlay(const LLUUID& id, LLVector3d position, S32 type, const LLUUID& assetid, const LLUUID& ownerid, const LLUUID& sourceid, bool is_trigger, bool is_looped);
    static void logSoundStop(const LLUUID& id);
    static void pruneSoundLog();
    static S32 sSoundHistoryPruneCounter;
    // NaCl End

protected:
    LLUUID          mID; // The ID of the source is that of the object if it's attached to an object.
    LLUUID          mOwnerID;   // owner of the object playing the sound
    F32             mPriority;
    F32             mGain;
    F32             mOcclusion = 0.f;   // <SS:Nexii> see setOcclusion
    U32             mStartOffsetMS = 0; // <SS:Nexii> see setStartOffsetMS
    bool            mSourceMuted;
    bool            mForcedPriority; // ignore mute, set high priority, researved for sound preview and UI
    bool            mLoop;
    bool            mSyncMaster;
    bool            mSyncSlave;
    bool            mQueueSounds;
    bool            mPlayedOnce;
    bool            mCorrupted;
    S32             mType;
    LLVector3d      mPositionGlobal;
    LLVector3       mVelocity;
    // NaCl - Sound explorer
    LLUUID          mLogID;
    LLUUID          mSourceID;
    bool            mIsTrigger;
    // NaCl end

    //LLAudioSource *mSyncMasterp;  // If we're a slave, the source that we're synced to.
    LLAudioChannel  *mChannelp;     // If we're currently playing back, this is the channel that we're assigned to.
    LLAudioData     *mCurrentDatap;
    LLAudioData     *mQueuedDatap;

    typedef std::map<LLUUID, LLAudioData *> data_map;
    data_map mPreloadMap;

    LLFrameTimer mAgeTimer;
};




//
// Generic metadata about a particular piece of audio data.
// The actual data is handled by the derived LLAudioBuffer classes which are
// derived for each audio engine.
//


class LLAudioData
{
  public:
    LLAudioData(const LLUUID &uuid);
    bool load();

    LLUUID         getID() const { return mID; }
    LLAudioBuffer *getBuffer() const { return mBufferp; }

    bool hasLocalData() const { return mHasLocalData; }
    bool hasDecodedData() const { return mHasDecodedData; }
    bool hasCompletedDecode() const { return mHasCompletedDecode; }
    bool hasDecodeFailed() const { return mHasDecodeFailed; }
    bool hasWAVLoadFailed() const { return mHasWAVLoadFailed; }

    void setHasLocalData(const bool hld) { mHasLocalData = hld; }
    void setHasDecodedData(const bool hdd) { mHasDecodedData = hdd; }
    void setHasCompletedDecode(const bool hcd) { mHasCompletedDecode = hcd; }
    void setHasDecodeFailed(const bool hdf) { mHasDecodeFailed = hdf; }
    void setHasWAVLoadFailed(const bool hwlf) { mHasWAVLoadFailed = hwlf; }

    friend class LLAudioEngine;  // Severe laziness, bad.

  protected:
    LLUUID         mID;
    LLAudioBuffer *mBufferp;             // If this data is being used by the audio system, a pointer to the buffer will be set here.
    bool           mHasLocalData;        // Set true if the encoded sound asset file is available locally
    bool           mHasDecodedData;      // Set true if the decoded sound file is available on disk
    bool           mHasCompletedDecode;  // Set true when the sound is decoded
    bool           mHasDecodeFailed;     // Set true if decoding failed, meaning the sound asset is bad
    bool mHasWAVLoadFailed;  // Set true if loading the decoded WAV file failed, meaning the sound asset should be decoded instead if
                             // possible
};


//
// Base class for an audio channel, i.e. a channel which is capable of playing back a sound.
// Management of channels is done generically, methods for actually manipulating the channel
// are derived for each audio engine.
//


class LLAudioChannel
{
public:
    LLAudioChannel();
    virtual ~LLAudioChannel();

    virtual void setSource(LLAudioSource *sourcep);
    LLAudioSource *getSource() const            { return mCurrentSourcep; }

    void setSecondaryGain(F32 gain)             { mSecondaryGain = gain; }
    F32 getSecondaryGain()                      { return mSecondaryGain; }

    friend class LLAudioEngine;
    friend class LLAudioSource;
protected:
    virtual void play() = 0;
    virtual void playSynced(LLAudioChannel *channelp) = 0;
    virtual void cleanup() = 0;
    virtual bool isPlaying() = 0;
    void setWaiting(const bool waiting)         { mWaiting = waiting; }
    bool isWaiting() const                      { return mWaiting; }

    virtual bool updateBuffer(); // Check to see if the buffer associated with the source changed, and update if necessary.
    virtual void update3DPosition() = 0;
    virtual void updateLoop() = 0; // Update your loop/completion status, for use by queueing/syncing.
    // <SS:Nexii> pcm byte position for in-flight loudness patches; engines without seek just restart
    virtual U32 getPositionBytes()                  { return 0; }
    virtual void setPositionBytes(U32 bytes)        { }
    // </SS:Nexii>
protected:
    LLAudioSource   *mCurrentSourcep;
    LLAudioBuffer   *mCurrentBufferp;
    bool            mLoopedThisFrame;
    bool            mWaiting;   // Waiting for sync.
    F32             mSecondaryGain;
};




// Basically an interface class to the engine-specific implementation
// of audio data that's ready for playback.
// Will likely get more complex as we decide to do stuff like real streaming audio.


class LLAudioBuffer
{
public:
    virtual ~LLAudioBuffer() {};
    virtual bool loadWAV(const std::string& filename) = 0;

    // Length in PCM BYTES - which is what it has always returned, and worth
    // saying out loud: read as any unit of time it is wrong by whatever the
    // sample rate and channel count happen to be.
    virtual U32 getLength() = 0;

    // Length in milliseconds, or 0 when the backend cannot say.
    //
    // Not pure, so a backend that has no answer simply does not override it
    // and callers get the honest zero rather than a build break.
    virtual U32 getLengthMS() { return 0; }

    // <SS:Nexii> Where the sound's main event begins, in milliseconds from the start, or 0 when the backend cannot say or there is no clear one. "Begins", not "peaks": what a listener hears as the moment a thunder clap happened is the ONSET of the bang, not the instant of greatest energy some way into it. A caller that wants a sound's bang to land at a particular time starts playback that much earlier. Exists because a recording rarely starts at its own event - there is leading air, a breath of room tone, a fade-in - and none of that is knowable from the asset UUID. Thunder is the case that needs it: the flash and the clap are separated by a delay computed from distance, and an unknown offset inside the file corrupts that delay by however long the file's own preamble happens to be.
    virtual U32 getOnsetMS() { return 0; }

    // RMS of the loudest ~1s of the sound, 0..1 against full scale, or 0 when the backend cannot say. The loudness proxy for levelling a pack of differently-mastered assets against each other:
    // whole-file RMS would be biased low by long echo tails and over-boost exactly the wrong files, so the loudest window is measured instead. Cached with the onset - one PCM pass fills both.
    virtual F32 getPeakLevel() { return 0.f; }

    // A plain copy of the decoded samples, for analysis on threads the backend's own objects must never touch. False when the format is not something worth guessing about.
    virtual bool getPCMCopy(std::vector<S16>& out, S32& out_channels, F32& out_rate) { return false; }

    friend class LLAudioEngine;
    friend class LLAudioChannel;
    friend class LLAudioData;
protected:
    bool mInUse;
    LLAudioData *mAudioDatap;
    LLFrameTimer mLastUseTimer;
};

struct SoundData
{
    LLUUID audio_uuid;
    LLUUID owner_id;
    F32 gain;
    S32 type;
    LLVector3d pos_global;

    SoundData(const LLUUID &audio_uuid,
        const LLUUID& owner_id,
        const F32 gain,
        const S32 type = LLAudioEngine::AUDIO_TYPE_NONE,
        const LLVector3d &pos_global = LLVector3d::zero) :
        audio_uuid(audio_uuid),
        owner_id(owner_id),
        gain(gain),
        type(type),
        pos_global(pos_global)
    {
    }
};

extern LLAudioEngine* gAudiop;

// NaCl - Sound explorer
struct LLSoundHistoryItem
{
    LLUUID mID;
    LLVector3d mPosition;
    S32 mType;
    bool mPlaying;
    LLUUID mAssetID;
    LLUUID mOwnerID;
    LLUUID mSourceID;
    bool mIsTrigger;
    bool mIsLooped;
    F64 mTimeStarted;
    F64 mTimeStopped;
    bool mReviewed;
    bool mReviewedCollision;

    LLSoundHistoryItem()
      : mType(0)
      , mPlaying(0)
      , mIsTrigger(0)
      , mIsLooped(0)
      , mTimeStarted(0.f)
      , mTimeStopped(0.f)
      , mReviewed(false)
      , mReviewedCollision(false)
    {
    }
};
extern std::map<LLUUID, LLSoundHistoryItem> gSoundHistory;
// NaCl End

#endif
