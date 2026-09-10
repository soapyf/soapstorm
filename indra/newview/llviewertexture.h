/**
 * @file llviewertexture.h
 * @brief Object for managing images and their textures
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

#ifndef LL_LLVIEWERTEXTURE_H
#define LL_LLVIEWERTEXTURE_H

#include "llatomic.h"
#include "llgltexture.h"
#include "lltimer.h"
#include "llframetimer.h"
#include "llhost.h"
#include "llgltypes.h"
#include "llrender.h"
#include "llmetricperformancetester.h"
#include "httpcommon.h"
#include "workqueue.h"
#include "gltf/common.h"

#include <map>
#include <list>

// <FS:Ansariel> Max texture resolution
constexpr F32 MIN_VRAM_BUDGET = 768.f; // <FS:Ansariel> Expose max texture VRAM setting

class LLFace;
class LLImageGL ;
class LLImageRaw;
class LLViewerObject;
class LLViewerTexture;
class LLViewerFetchedTexture ;
class LLViewerMediaTexture ;
class LLTexturePipelineTester ;


typedef void    (*loaded_callback_func)( bool success, LLViewerFetchedTexture *src_vi, LLImageRaw* src, LLImageRaw* src_aux, S32 discard_level, bool final, void* userdata );

class LLFileSystem;
class LLMessageSystem;
class LLViewerMediaImpl ;
class LLVOVolume ;
struct LLTextureKey;

class LLLoadedCallbackEntry
{
public:
    typedef std::set< LLTextureKey > source_callback_list_t;

public:
    LLLoadedCallbackEntry(loaded_callback_func cb,
                          S32 discard_level,
                          bool need_imageraw, // Needs image raw for the callback
                          void* userdata,
                          source_callback_list_t* src_callback_list,
                          LLViewerFetchedTexture* target,
                          bool pause);
    ~LLLoadedCallbackEntry();
    void removeTexture(LLViewerFetchedTexture* tex) ;

    loaded_callback_func    mCallback;
    S32                     mLastUsedDiscard;
    S32                     mDesiredDiscard;
    bool                    mNeedsImageRaw;
    bool                    mPaused;
    void*                   mUserData;
    source_callback_list_t* mSourceCallbackList;

public:
    static void cleanUpCallbackList(LLLoadedCallbackEntry::source_callback_list_t* callback_list) ;
};

class LLTextureBar;

class LLViewerTexture : public LLGLTexture
{
public:
    enum
    {
        LOCAL_TEXTURE,
        MEDIA_TEXTURE,
        DYNAMIC_TEXTURE,
        FETCHED_TEXTURE,
        LOD_TEXTURE,
        INVALID_TEXTURE_TYPE
    };

    typedef std::vector<class LLFace*> ll_face_list_t;
    typedef std::vector<LLVOVolume*> ll_volume_list_t;


protected:
    virtual ~LLViewerTexture();
    LOG_CLASS(LLViewerTexture);

public:
    static void initClass();
    static void updateClass();
    static bool isSystemMemoryLow();
    static bool isSystemMemoryCritical();

    LLViewerTexture(bool usemipmaps = true);
    LLViewerTexture(const LLUUID& id, bool usemipmaps) ;
    LLViewerTexture(const LLImageRaw* raw, bool usemipmaps) ;
    LLViewerTexture(const U32 width, const U32 height, const U8 components, bool usemipmaps) ;

    virtual S8 getType() const;
    virtual bool isMissingAsset() const ;
    virtual void dump();    // debug info to LL_INFOS()

    virtual bool isViewerMediaTexture() const { return false; }

    /*virtual*/ bool bindDefaultImage(const S32 stage = 0) ;
    /*virtual*/ bool bindDebugImage(const S32 stage = 0) ;
    /*virtual*/ void forceImmediateUpdate() ;
    /*virtual*/ bool isActiveFetching();

    /*virtual*/ const LLUUID& getID() const { return mID; }
    virtual void setBoostLevel(S32 level);
    S32  getBoostLevel() { return mBoostLevel; }
    void setTextureListType(S32 tex_type) { mTextureListType = tex_type; }
    S32 getTextureListType() { return mTextureListType; }

    void addTextureStats(F32 virtual_size, bool needs_gltexture = true) const;
    void resetTextureStats();
    void setMaxVirtualSizeResetInterval(S32 interval)const {mMaxVirtualSizeResetInterval = interval;}
    void resetMaxVirtualSizeResetCounter()const {mMaxVirtualSizeResetCounter = mMaxVirtualSizeResetInterval;}
    S32 getMaxVirtualSizeResetCounter() const { return mMaxVirtualSizeResetCounter; }

    virtual F32  getMaxVirtualSize() ;

    LLFrameTimer* getLastReferencedTimer() { return &mLastReferencedTimer; }

    S32 getFullWidth() const { return mFullWidth; }
    S32 getFullHeight() const { return mFullHeight; }
    /*virtual*/ void setKnownDrawSize(S32 width, S32 height);

    virtual void addFace(U32 channel, LLFace* facep) ;
    virtual void removeFace(U32 channel, LLFace* facep) ;
    S32 getTotalNumFaces() const;
    S32 getNumFaces(U32 ch) const;
    const ll_face_list_t* getFaceList(U32 channel) const {llassert(channel < LLRender::NUM_TEXTURE_CHANNELS); return &mFaceList[channel];}

    virtual void addVolume(U32 channel, LLVOVolume* volumep);
    virtual void removeVolume(U32 channel, LLVOVolume* volumep);
    S32 getNumVolumes(U32 channel) const;
    const ll_volume_list_t* getVolumeList(U32 channel) const { return &mVolumeList[channel]; }

    bool isLargeImage() ;
    bool isInvisiprim() const;
    static bool isInvisiprim(const LLUUID& id);

    void setParcelMedia(LLViewerMediaTexture* media) {mParcelMedia = media;}
    bool hasParcelMedia() const { return mParcelMedia != NULL;}
    LLViewerMediaTexture* getParcelMedia() const { return mParcelMedia;}

    /*virtual*/ void updateBindStatsForTester() ;

    struct MaterialEntry
    {
        S32 mIndex = LL::GLTF::INVALID_INDEX;
        std::shared_ptr<LL::GLTF::Asset> mAsset;
    };
    typedef std::vector<MaterialEntry> material_list_t;
    material_list_t   mMaterialList;  // reverse pointer pointing to LL::GLTF::Materials using this image as texture

protected:
    void cleanup() ;
    void init(bool firstinit) ;
    void reorganizeFaceList() ;
    void reorganizeVolumeList();

private:
    friend class LLBumpImageList;
    friend class LLUIImageList;

    static U32Megabytes getFreeSystemMemory();

protected:
    friend class LLViewerTextureList;
    LLUUID mID;
    S32 mTextureListType; // along with mID identifies where to search for this texture in TextureList

    mutable F32 mMaxVirtualSize = 0.f;  // The largest virtual size of the image, in pixels - how much data to we need?
    mutable S32  mMaxVirtualSizeResetCounter;
    mutable S32  mMaxVirtualSizeResetInterval;
    LLFrameTimer mLastReferencedTimer;

    ll_face_list_t    mFaceList[LLRender::NUM_TEXTURE_CHANNELS]; //reverse pointer pointing to the faces using this image as texture
    U32               mNumFaces[LLRender::NUM_TEXTURE_CHANNELS];
    LLFrameTimer      mLastFaceListUpdateTimer ;

    ll_volume_list_t  mVolumeList[LLRender::NUM_VOLUME_TEXTURE_CHANNELS];
    U32                 mNumVolumes[LLRender::NUM_VOLUME_TEXTURE_CHANNELS];
    LLFrameTimer      mLastVolumeListUpdateTimer;

    //do not use LLPointer here.
    LLViewerMediaTexture* mParcelMedia ;

    LL::WorkQueue::weak_t mMainQueue;
    LL::WorkQueue::weak_t mImageQueue;

public:
    static const U32 sCurrentFileVersion;
    static S32 sImageCount;
    static S32 sRawCount;
    static S32 sAuxCount;
    static LLFrameTimer sEvaluationTimer;
    static F32 sDesiredDiscardBias;
    static U32 sBiasTexturesUpdated;
    static S32 sMaxSculptRez ;
    static U32 sMinLargeImageSize ;
    static U32 sMaxSmallImageSize ;
    static bool sFreezeImageUpdates;
    static F32  sCurrentTime ;
    static LLUUID sInvisiprimTexture1 ;
    static LLUUID sInvisiprimTexture2 ;

    // estimated free memory for textures, by bias calculation
    static F32 sFreeVRAMMegabytes;

    enum EDebugTexels
    {
        DEBUG_TEXELS_OFF,
        DEBUG_TEXELS_CURRENT,
        DEBUG_TEXELS_DESIRED,
        DEBUG_TEXELS_FULL
    };

    static EDebugTexels sDebugTexelsMode;

    static LLPointer<LLViewerTexture> sNullImagep; // Null texture for non-textured objects.
    static LLPointer<LLViewerTexture> sBlackImagep; // Texture to show NOTHING (pure black)
    static LLPointer<LLViewerTexture> sCheckerBoardImagep;  // Texture to show NOTHING (pure black)
};


enum FTType
{
    FTT_UNKNOWN = -1,
    FTT_DEFAULT = 0, // standard texture fetched by id.
    FTT_SERVER_BAKE, // texture produced by appearance service and fetched from there.
    FTT_HOST_BAKE, // old-style baked texture uploaded by viewer and fetched from avatar's host.
    FTT_MAP_TILE, // tiles are fetched from map server directly.
    FTT_LOCAL_FILE // fetch directly from a local file.
};

const std::string& fttype_to_string(const FTType& fttype);

//
//textures are managed in gTextureList.
//raw image data is fetched from remote or local cache
//but the raw image this texture pointing to is fixed.
//
class LLViewerFetchedTexture : public LLViewerTexture
{
    friend class LLTextureBar; // debug info only
    friend class LLTextureView; // debug info only

protected:
    /*virtual*/ ~LLViewerFetchedTexture();
public:
    LLViewerFetchedTexture(const LLUUID& id, FTType f_type, const LLHost& host = LLHost(), bool usemipmaps = true);
    LLViewerFetchedTexture(const LLImageRaw* raw, FTType f_type, bool usemipmaps);
    LLViewerFetchedTexture(const std::string& url, FTType f_type, const LLUUID& id, bool usemipmaps = true);

public:

    struct Compare
    {
        // lhs < rhs
        bool operator()(const LLPointer<LLViewerFetchedTexture> &lhs, const LLPointer<LLViewerFetchedTexture> &rhs) const
        {
            const LLViewerFetchedTexture* lhsp = (const LLViewerFetchedTexture*)lhs;
            const LLViewerFetchedTexture* rhsp = (const LLViewerFetchedTexture*)rhs;

            // greater priority is "less"
            const F32 lpriority = lhsp->mMaxVirtualSize;
            const F32 rpriority = rhsp->mMaxVirtualSize;
            if (lpriority > rpriority) // higher priority
                return true;
            if (lpriority < rpriority)
                return false;
            return lhsp < rhsp;
        }
    };

public:
    /*virtual*/ S8 getType() const override;
    FTType getFTType() const;
    /*virtual*/ void forceImmediateUpdate() override;
    /*virtual*/ void dump() override;

    // Set callbacks to get called when the image gets updated with higher
    // resolution versions.
    void setLoadedCallback(loaded_callback_func cb,
                           S32 discard_level, bool keep_imageraw, bool needs_aux,
                           void* userdata, LLLoadedCallbackEntry::source_callback_list_t* src_callback_list, bool pause = false);
    bool hasCallbacks() { return !mLoadedCallbackList.empty(); }
    void pauseLoadedCallbacks(const LLLoadedCallbackEntry::source_callback_list_t* callback_list);
    void unpauseLoadedCallbacks(const LLLoadedCallbackEntry::source_callback_list_t* callback_list);
    bool doLoadedCallbacks();
    void deleteCallbackEntry(const LLLoadedCallbackEntry::source_callback_list_t* callback_list);
    void clearCallbackEntryList() ;

    void addToCreateTexture();

    //call to determine if createTexture is necessary
    bool preCreateTexture(S32 usename = 0);
     // ONLY call from LLViewerTextureList or ImageGL background thread
    bool createTexture(S32 usename = 0);
    void postCreateTexture();
    void scheduleCreateTexture();

    void destroyTexture() ;

    virtual void processTextureStats() ;

    bool needsAux() const { return mNeedsAux; }

    // Host we think might have this image, used for baked av textures.
    void setTargetHost(LLHost host)         { mTargetHost = host; }
    LLHost getTargetHost() const            { return mTargetHost; }

    void updateVirtualSize() ;

    S32  getDesiredDiscardLevel()            { return mDesiredDiscardLevel; }
    void setMinDiscardLevel(S32 discard)    { mMinDesiredDiscardLevel = llmin(mMinDesiredDiscardLevel,(S8)discard); }

    void setBoostLevel(S32 level) override;
    bool updateFetch();

    void clearFetchedResults(); //clear all fetched results, for debug use.

    // Override the computation of discard levels if we know the exact output
    // size of the image.  Used for UI textures to not decode, even if we have
    // more data.
    /*virtual*/ void setKnownDrawSize(S32 width, S32 height) override;

    // Set the debug text of all Viewer Objects associated with this texture
    // to the specified text
    void setDebugText(const std::string& text);

    void setIsMissingAsset(bool is_missing = true);
    /*virtual*/ bool isMissingAsset() const override { return mIsMissingAsset; }

    // returns dimensions of original image for local files (before power of two scaling)
    // and returns 0 for all asset system images
    S32 getOriginalWidth() { return mOrigWidth; }
    S32 getOriginalHeight() { return mOrigHeight; }

    bool isInImageList() const {return mInImageList ;}
    void setInImageList(bool flag) {mInImageList = flag ;}

    LLFrameTimer* getLastPacketTimer() {return &mLastPacketTimer;}

    U32 getFetchPriority() const { return mFetchPriority ;}
    F32 getDownloadProgress() const {return mDownloadProgress ;}

    void destroyRawImage();
    bool needsToSaveRawImage();

    const std::string& getUrl() const {return mUrl;}
    //---------------
    bool isDeleted() ;
    bool getUseDiscard() const { return mUseMipMaps && !mDontDiscard; }
    //---------------

    void setForSculpt();
    bool forSculpt() const {return mForSculpt;}
    bool isForSculptOnly() const;

    //raw image management
    LLImageRaw* getRawImage()const { return mRawImage ;}
    S32         getRawImageLevel() const {return mRawDiscardLevel;}
    bool        isRawImageValid()const { return mIsRawImageValid ; }
    void        forceToSaveRawImage(S32 desired_discard = 0, F32 kept_time = 0.f) ;

    // readback the raw image from OpenGL if mRawImage is not valid
    void        readbackRawImage();

    void        destroySavedRawImage() ;
    LLImageRaw* getSavedRawImage() ;
    S32         getSavedRawImageLevel() const {return mSavedRawDiscardLevel; }

    const LLImageRaw* getSavedRawImage() const;
    const LLImageRaw* getAuxRawImage() const { return mAuxRawImage; }
    bool        hasSavedRawImage() const ;
    F32         getElapsedLastReferencedSavedRawImageTime() const ;
    bool        isFullyLoaded() const;

    bool        hasFetcher() const { return mHasFetcher;}
    bool        isFetching() const { return mIsFetching;}
    void        setCanUseHTTP(bool can_use_http) {mCanUseHTTP = can_use_http;}

    void        forceToDeleteRequest();
    void        loadFromFastCache();
    void        setInFastCacheList(bool in_list) { mInFastCacheList = in_list; }
    bool        isInFastCacheList() { return mInFastCacheList; }

    /*virtual*/bool  isActiveFetching() override; //is actively in fetching by the fetching pipeline.

    virtual bool scaleDown() { return false; };

    // <SS:Nexii> Squeeze - the BC7 residency ladder, see doc/super_compressed_textures.md. It lives on this class and not on LLImageGL for three reasons drawn from the code: this is the only object that survives the whole fetch/create cycle, updateFetch has to be able to see the state to decide whether to spend network on the J2C copy, and both of the edges verification found bugs on - RESIDENT to raw-needed and RESIDENT to uncompressed-upgrade - are member functions here.
    //
    // DECLINED is a fifth, terminal value beyond the four the design named, and it carries the reason. Without it a texture excluded for sculpt or raw-consumer reasons would be re-probed every frame, and the log could not tell "never considered" from "considered and refused" - the exact failure the encode side already paid for and fixed with ESSBC7EncodeVerdict.
    enum ESSBC7Residency
    {
        SSBC7_RES_NONE = 0,
        SSBC7_RES_HIT_KNOWN,
        SSBC7_RES_READING,
        SSBC7_RES_RESIDENT,
        SSBC7_RES_DECLINED
    };

    U8   ssBC7Residency() const           { return mSSBC7Residency; }
    void ssBC7SetResidency(U8 state)      { mSSBC7Residency = state; }
    U8   ssBC7DeclineReason() const       { return mSSBC7DeclineReason; }

    // Out of line because it has to hand this texture's share of the live video-memory gauge back first. The ladder alone is not a safe key for that: a resident whose re-serve read fails passes through READING on its way to DECLINED, and keying the release on the ladder would miss it and drift the gauge upward for the rest of the session.
    void ssBC7SetDeclined(U8 reason);
    S32  ssBC7ServedDiscard() const       { return mSSBC7ServedDiscard; }
    bool ssBC7IsResident() const          { return mSSBC7Residency == (U8)SSBC7_RES_RESIDENT && mGLTexturep.notNull() && mGLTexturep->getHasGLTexture(); }

    // Latched at construction time, not re-tested later: the read path sets an explicit format itself, after which getHasExplicitFormat() can no longer distinguish "the caller demanded a format" from "we chose BPTC".
    void ssBC7LatchExplicitFormat()       { mSSBC7ExplicitFormat = true; }
    bool ssBC7HadExplicitFormat() const   { return mSSBC7ExplicitFormat; }

    // True when any loaded callback wants the LLImageRaw itself. mNeedsImageRaw is private to the entry, so this is the only way to ask.
    bool ssBC7NeedsRawCallback() const;

    // An uncompressed create is queued or already running for this texture. Both flags are needed: mCreatePending is the main-thread queue and mNeedsCreateTexture covers the LLImageGLThread route, which does the GL work on a worker and would otherwise be touching the same LLImageGL as a BC7 upload. Not const because LLAtomicBase's conversion operator is not.
    bool ssBC7CreateInFlight()            { return mNeedsCreateTexture || mCreatePending; }

    // Re-reads the alpha-mask verdict after an upload and dirties every face when it differs from the one the faces were built with; canRenderAsMask is baked into the pass assignment at genDrawInfo time, so a verdict that flips between discard levels reaches nothing without this. Main thread only. doc/alpha_mask_verdict.md
    void ssSyncAlphaMaskVerdict();

    // Uploads a stored BC7 mip prefix. Mirrors by hand what LLGLTexture::createGLTexture(discard, imageraw, ...) does around the raw overload, because there is no LLGLTexture wrapper for the data_hasmips form and processTextureStats divides by mTexelsPerImage.
    // <SS:Nexii/> Squeeze pick masks - `pick_mask` is the stored per-texel click mask for the FULL base level, or null for an opaque texture; installed on the LLImageGL after the compressed upload, which cannot build its own.
    bool ssBC7UploadFromStore(const U8* data_in, S32 serve_discard, S32 full_width, S32 full_height, S32 src_components, S32 mip_count, bool alpha_is_mask,
                              const U8* pick_mask = nullptr, U32 pick_mask_bytes = 0);

    void ssBC7NoteResident(S32 served_discard, U32 bc7_bytes, U32 saved_bytes);

    // The RESIDENT to uncompressed-upgrade edge, made a declared transition rather than something the format guard in llimagegl.cpp discovers. `verdict` is an ESSBC7ServeVerdict, kept as a U8 so this header does not have to drag in ssbc7serve.h.
    //
    // drop_format is false for the raw-consumer edges, which merely ARRANGE for an uncompressed image to be fetched later. Dropping the format there would leave the format fields describing RGBA8 while the GL object still holds BPTC levels, and everything that reasons from the format - scaleDown above all - would then be reasoning about a texture that does not exist. The drop happens at addToCreateTexture, which every uncompressed upload actually passes through.
    void ssBC7LeaveResidency(U8 verdict, const char* reason, bool drop_format = true);
    // </SS:Nexii>

    bool mCreatePending = false;    // if true, this is in gTextureList.mCreateTextureList
    mutable bool mDownScalePending = false; // if true, this is in gTextureList.mDownScaleQueue

    // <FS:Techwolf Lupindo> texture comment decoder
    std::map<std::string,std::string> mComment;
    // </FS:Techwolf Lupindo>

protected:
    S32 getCurrentDiscardLevelForFetching() ;
public: // <FS:Ansariel> Needed for texture refresh
    void forceToRefetchTexture(S32 desired_discard = 0, F32 kept_time = 60.f);

private:
    void init(bool firstinit) ;
    void cleanup() ;

    bool processFetchResults(S32& desired_discard, S32 current_discard, S32 fetch_discard, F32 decode_priority);

    void saveRawImage() ;

    // <SS:Nexii> Squeeze - the ONE place this texture's contribution to the live gauge is given back, keyed on the recorded byte counts rather than on the ladder state so that every route out of residency releases exactly once
    void ssBC7ReleaseGauge();
    // </SS:Nexii>

private:
    bool  mFullyLoaded;
    bool  mInFastCacheList;
    bool  mForceCallbackFetch;

protected:
    S32 mOrigWidth;
    S32 mOrigHeight;

    // Override the computation of discard levels if we know the exact output size of the image.
    // Used for UI textures to not decode, even if we have more data.
    S32 mKnownDrawWidth;
    S32 mKnownDrawHeight;
    bool mKnownDrawSizeChanged ;
    std::string mUrl;

    S32 mLastWorkerDiscardLevel;
    S32 mRequestedDiscardLevel;
    F32 mRequestedDownloadPriority;
    S32 mFetchState;
    S32 mLastFetchState = -1; // DEBUG
    U32 mFetchPriority;
    F32 mDownloadProgress;
    F32 mFetchDeltaTime;
    F32 mRequestDeltaTime;
    S32 mMinDiscardLevel;
    S8  mDesiredDiscardLevel;           // The discard level we'd LIKE to have - if we have it and there's space
    S8  mMinDesiredDiscardLevel;    // The minimum discard level we'd like to have

    bool mNeedsAux;                 // We need to decode the auxiliary channels
    bool mHasAux;                    // We have aux channels
    bool mDecodingAux;              // Are we decoding high components
    bool mIsRawImageValid;
    bool mHasFetcher;               // We've made a fecth request
    bool mIsFetching;               // Fetch request is active
    bool mCanUseHTTP;              //This texture can be fetched through http if true.
    LLCore::HttpStatus mLastHttpGetStatus; // Result of the most recently completed http request for this texture.

    FTType mFTType; // What category of image is this - map tile, server bake, etc?
    mutable bool mIsMissingAsset;       // True if we know that there is no image asset with this image id in the database.

    typedef std::list<LLLoadedCallbackEntry*> callback_list_t;
    S8              mLoadedCallbackDesiredDiscardLevel;
    bool            mPauseLoadedCallBacks;
    callback_list_t mLoadedCallbackList;
    F32             mLastCallBackActiveTime;

    LLPointer<LLImageRaw> mRawImage;
    S32 mRawDiscardLevel = -1;

    // Used ONLY for cloth meshes right now.  Make SURE you know what you're
    // doing if you use it for anything else! - djs
    LLPointer<LLImageRaw> mAuxRawImage;

    //keep a copy of mRawImage for some special purposes
    //when mForceToSaveRawImage is set.
    bool mForceToSaveRawImage ;
    bool mSaveRawImage;
    LLPointer<LLImageRaw> mSavedRawImage;
    S32 mSavedRawDiscardLevel;
    S32 mDesiredSavedRawDiscardLevel;
    F32 mLastReferencedSavedRawImageTime ;
    F32 mKeptSavedRawImageTime ;

    LLHost mTargetHost; // if invalid, just request from agent's simulator

    // Timers
    LLFrameTimer mLastPacketTimer;      // Time since last packet.
    LLFrameTimer mStopFetchingTimer;    // Time since mDecodePriority == 0.f.

    bool  mInImageList;             // true if image is in list (in which case don't reset priority!)
    // This needs to be atomic, since it is written both in the main thread
    // and in the GL image worker thread... HB
    LLAtomicBool  mNeedsCreateTexture;
    bool   mSSFaceMaskVerdict;      // <SS:Nexii/> the getIsAlphaMask() answer the faces last built against, see ssSyncAlphaMaskVerdict

    bool   mForSculpt ; //a flag if the texture is used as sculpt data.
    bool   mIsFetched ; //is loaded from remote or from cache, not generated locally.

    // <SS:Nexii> Squeeze - packed deliberately: there is one LLViewerFetchedTexture per texture the session has ever touched, so the ladder is four bytes of state plus the two byte counts the live video-memory gauge needs to be a gauge rather than a high-water mark.
    U8   mSSBC7Residency;
    U8   mSSBC7DeclineReason;
    S8   mSSBC7ServedDiscard;       // -1 whenever the ladder is not at RESIDENT
    bool mSSBC7ExplicitFormat;
    U32  mSSBC7ServedBytes;
    U32  mSSBC7SavedBytes;
    // </SS:Nexii>

public:
    static F32 sMaxVirtualSize; //maximum possible value of mMaxVirtualSize
    static LLPointer<LLViewerFetchedTexture> sMissingAssetImagep;   // Texture to show for an image asset that is not in the database
    static LLPointer<LLViewerFetchedTexture> sWhiteImagep;  // Texture to show NOTHING (whiteness)
    static LLPointer<LLViewerFetchedTexture> sDefaultImagep; // "Default" texture for error cases, the only case of fetched texture which is generated in local.
    static LLPointer<LLViewerFetchedTexture> sFlatNormalImagep; // Flat normal map denoting no bumpiness on a surface
    static LLPointer<LLViewerFetchedTexture> sDefaultIrradiancePBRp; // PBR: irradiance
    static LLPointer<LLViewerFetchedTexture> sDefaultParticleImagep; // Default particle texture

    // not sure why, but something is iffy about the loading of this particular texture, use the accessor instead of accessing directly
    static LLPointer<LLViewerFetchedTexture> sSmokeImagep; // Old "Default" translucent texture
    static LLViewerFetchedTexture* getSmokeImage();

// [SL:KB] - Patch: Render-TextureToggle (Catznip-4.0)
    static LLPointer<LLViewerFetchedTexture> sDefaultDiffuseImagep;
// [/SL:KB]
};

//
//the image data is fetched from remote or from local cache
//the resolution of the texture is adjustable: depends on the view-dependent parameters.
//
class LLViewerLODTexture : public LLViewerFetchedTexture
{
protected:
    /*virtual*/ ~LLViewerLODTexture(){}

public:
    LLViewerLODTexture(const LLUUID& id, FTType f_type, const LLHost& host = LLHost(), bool usemipmaps = true);
    LLViewerLODTexture(const std::string& url, FTType f_type, const LLUUID& id, bool usemipmaps = true);

    S8 getType() const override;
    // Process image stats to determine priority/quality requirements.
    void processTextureStats() override;
    bool isUpdateFrozen() ;

    bool scaleDown() override;

private:
    void init(bool firstinit) ;
};

//
//the image data is fetched from the media pipeline periodically
//the resolution of the texture is also adjusted by the media pipeline
//
class LLViewerMediaTexture : public LLViewerTexture
{
protected:
    /*virtual*/ ~LLViewerMediaTexture() ;

public:
    LLViewerMediaTexture(const LLUUID& id, bool usemipmaps = true, LLImageGL* gl_image = NULL) ;

    /*virtual*/ S8 getType() const;
    void reinit(bool usemipmaps = true);

    bool  getUseMipMaps() {return mUseMipMaps ; }
    void  setUseMipMaps(bool mipmap) ;

    void setPlaying(bool playing) ;
    bool isPlaying() const {return mIsPlaying;}
    void setMediaImpl() ;

    virtual bool isViewerMediaTexture() const { return true; }

    void initVirtualSize() ;
    void invalidateMediaImpl() ;

    void addMediaToFace(LLFace* facep) ;
    void removeMediaFromFace(LLFace* facep) ;

    /*virtual*/ void addFace(U32 ch, LLFace* facep) ;
    /*virtual*/ void removeFace(U32 ch, LLFace* facep) ;

    /*virtual*/ F32  getMaxVirtualSize();

private:
    void switchTexture(U32 ch, LLFace* facep) ;
    bool findFaces() ;
    void stopPlaying() ;

private:
    //
    //an instant list, recording all faces referencing or can reference to this media texture.
    //NOTE: it is NOT thread safe.
    //
    std::list< LLFace* > mMediaFaceList ;

    //an instant list keeping all textures which are replaced by the current media texture,
    //is only used to avoid the removal of those textures from memory.
    std::list< LLPointer<LLViewerTexture> > mTextureList ;

    LLViewerMediaImpl* mMediaImplp ;
    bool mIsPlaying ;
    U32  mUpdateVirtualSizeTime ;

public:
    static void updateClass() ;
    static void cleanUpClass() ;

    static LLViewerMediaTexture* findMediaTexture(const LLUUID& media_id) ;
    static void removeMediaImplFromTexture(const LLUUID& media_id) ;

private:
    typedef std::map< LLUUID, LLPointer<LLViewerMediaTexture> > media_map_t ;
    static media_map_t sMediaMap ;
};

//just an interface class, do not create instance from this class.
class LLViewerTextureManager
{
private:
    //make the constructor private to preclude creating instances from this class.
    LLViewerTextureManager(){}

public:
    //texture pipeline tester
    static LLTexturePipelineTester* sTesterp ;

    //returns NULL if tex is not a LLViewerFetchedTexture nor derived from LLViewerFetchedTexture.
    static LLViewerFetchedTexture*    staticCastToFetchedTexture(LLTexture* tex, bool report_error = false) ;

    //
    //"find-texture" just check if the texture exists, if yes, return it, otherwise return null.
    //
    static void                       findFetchedTextures(const LLUUID& id, std::vector<LLViewerFetchedTexture*> &output);
    static void                       findTextures(const LLUUID& id, std::vector<LLViewerTexture*> &output);
    static LLViewerFetchedTexture*    findFetchedTexture(const LLUUID& id, S32 tex_type);
    static LLViewerMediaTexture*      findMediaTexture(const LLUUID& id) ;

    static LLViewerMediaTexture*      createMediaTexture(const LLUUID& id, bool usemipmaps = true, LLImageGL* gl_image = NULL) ;

    //
    //"get-texture" will create a new texture if the texture does not exist.
    //
    static LLViewerMediaTexture*      getMediaTexture(const LLUUID& id, bool usemipmaps = true, LLImageGL* gl_image = NULL) ;

    static LLPointer<LLViewerTexture> getLocalTexture(bool usemipmaps = true, bool generate_gl_tex = true);
    static LLPointer<LLViewerTexture> getLocalTexture(const LLUUID& id, bool usemipmaps, bool generate_gl_tex = true) ;
    static LLPointer<LLViewerTexture> getLocalTexture(const LLImageRaw* raw, bool usemipmaps) ;
    static LLPointer<LLViewerTexture> getLocalTexture(const U32 width, const U32 height, const U8 components, bool usemipmaps, bool generate_gl_tex = true) ;

    static LLViewerFetchedTexture* getFetchedTexture(const LLImageRaw* raw, FTType type, bool usemipmaps);

    static LLViewerFetchedTexture* getFetchedTexture(const LLUUID &image_id,
                                     FTType f_type = FTT_DEFAULT,
                                     bool usemipmap = true,
                                     LLViewerTexture::EBoostLevel boost_priority = LLGLTexture::BOOST_NONE,     // Get the requested level immediately upon creation.
                                     S8 texture_type = LLViewerTexture::FETCHED_TEXTURE,
                                     LLGLint internal_format = 0,
                                     LLGLenum primary_format = 0,
                                     LLHost request_from_host = LLHost()
                                     );

    static LLViewerFetchedTexture* getFetchedTextureFromFile(const std::string& filename,
                                     FTType f_type = FTT_LOCAL_FILE,
                                     bool usemipmap = true,
                                     LLViewerTexture::EBoostLevel boost_priority = LLGLTexture::BOOST_NONE,
                                     S8 texture_type = LLViewerTexture::FETCHED_TEXTURE,
                                     LLGLint internal_format = 0,
                                     LLGLenum primary_format = 0,
                                     const LLUUID& force_id = LLUUID::null
                                     );

    static LLViewerFetchedTexture* getFetchedTextureFromUrl(const std::string& url,
                                     FTType f_type,
                                     bool usemipmap = true,
                                     LLViewerTexture::EBoostLevel boost_priority = LLGLTexture::BOOST_NONE,
                                     S8 texture_type = LLViewerTexture::FETCHED_TEXTURE,
                                     LLGLint internal_format = 0,
                                     LLGLenum primary_format = 0,
                                     const LLUUID& force_id = LLUUID::null
                                     );

    static LLViewerFetchedTexture* getFetchedTextureFromHost(const LLUUID& image_id, FTType f_type, LLHost host) ;

    // decode a given image data according to given mime type
    // WARNING: caller is responsible for deleting the returned raw image
    static LLImageRaw* getRawImageFromMemory(const U8* data, U32 size, std::string_view mimetype);

    // decode given image data according to given mime type
    // WARNING: caller is responsible for deleting the returned image
    static LLViewerFetchedTexture* getFetchedTextureFromMemory(const U8* data, U32 size, std::string_view mimetype);

    static void init() ;
    static void cleanup() ;
};
//
//this class is used for test/debug only
//it tracks the activities of the texture pipeline
//records them, and outputs them to log files
//
class LLTexturePipelineTester : public LLMetricPerformanceTesterWithSession
{
    enum
    {
        MIN_LARGE_IMAGE_AREA = 262144  //512 * 512
    };
public:
    LLTexturePipelineTester() ;
    ~LLTexturePipelineTester() ;

    void update();
    void updateTextureBindingStats(const LLViewerTexture* imagep) ;
    void updateTextureLoadingStats(const LLViewerFetchedTexture* imagep, const LLImageRaw* raw_imagep, bool from_cache) ;
    void updateGrayTextureBinding() ;
    void setStablizingTime() ;

private:
    void reset() ;
    void updateStablizingTime() ;

    /*virtual*/ void outputTestRecord(LLSD* sd) ;

private:
    bool mPause ;
private:
    bool mUsingDefaultTexture;            //if set, some textures are still gray.

    U32Bytes mTotalBytesUsed ;                     //total bytes of textures bound/used for the current frame.
    U32Bytes mTotalBytesUsedForLargeImage ;        //total bytes of textures bound/used for the current frame for images larger than 256 * 256.
    U32Bytes mLastTotalBytesUsed ;                 //total bytes of textures bound/used for the previous frame.
    U32Bytes mLastTotalBytesUsedForLargeImage ;    //total bytes of textures bound/used for the previous frame for images larger than 256 * 256.

    //
    //data size
    //
    U32Bytes mTotalBytesLoaded ;               //total bytes fetched by texture pipeline
    U32Bytes mTotalBytesLoadedFromCache ;      //total bytes fetched by texture pipeline from local cache
    U32Bytes mTotalBytesLoadedForLargeImage ;  //total bytes fetched by texture pipeline for images larger than 256 * 256.
    U32Bytes mTotalBytesLoadedForSculpties ;   //total bytes fetched by texture pipeline for sculpties

    //
    //time
    //NOTE: the error tolerances of the following timers is one frame time.
    //
    F32 mStartFetchingTime ;
    F32 mTotalGrayTime ;                  //total loading time when no gray textures.
    F32 mTotalStablizingTime ;            //total stablizing time when texture memory overflows
    F32 mStartTimeLoadingSculpties ;      //the start moment of loading sculpty images.
    F32 mEndTimeLoadingSculpties ;        //the end moment of loading sculpty images.
    F32 mStartStablizingTime ;
    F32 mEndStablizingTime ;

private:
    //
    //The following members are used for performance analyzing
    //
    class LLTextureTestSession : public LLTestSession
    {
    public:
        LLTextureTestSession() ;
        /*virtual*/ ~LLTextureTestSession() ;

        void reset() ;

        F32 mTotalGrayTime ;
        F32 mTotalStablizingTime ;
        F32 mStartTimeLoadingSculpties ;
        F32 mTotalTimeLoadingSculpties ;

        S32 mTotalBytesLoaded ;
        S32 mTotalBytesLoadedFromCache ;
        S32 mTotalBytesLoadedForLargeImage ;
        S32 mTotalBytesLoadedForSculpties ;

        typedef struct _texture_instant_preformance_t
        {
            S32 mAverageBytesUsedPerSecond ;
            S32 mAverageBytesUsedForLargeImagePerSecond ;
            F32 mAveragePercentageBytesUsedPerSecond ;
            F32 mTime ;
        }texture_instant_preformance_t ;
        std::vector<texture_instant_preformance_t> mInstantPerformanceList ;
        S32 mInstantPerformanceListCounter ;
    };

    /*virtual*/ LLMetricPerformanceTesterWithSession::LLTestSession* loadTestSession(LLSD* log) ;
    /*virtual*/ void compareTestSessions(llofstream* os) ;
};

#endif
