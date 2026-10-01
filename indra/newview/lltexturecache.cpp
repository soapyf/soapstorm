/**
 * @file lltexturecache.cpp
 * @brief Object which handles local texture caching
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

#include "llviewerprecompiledheaders.h"

#include "lltexturecache.h"

#include "llapr.h"
#include "lldir.h"
#include "llimage.h"
#include "llimagej2c.h" // for version control
#include "lllfsthread.h"
#include "llviewercontrol.h"

// Included to allow LLTextureCache::purgeTextures() to pause watchdog timeout
#include "llappviewer.h"
#include "llmemory.h"
#include "ssbc7promote.h" // <SS:Nexii> Squeeze promotion - ssBC7J2CExtent, so the partial-sentinel rule has exactly one implementation and it is the one the offline harness runs
#include "ssbc7store.h" // <SS:Nexii> Squeeze
#include "ssstrata.h"   // <SS:Nexii> Strata - the J2C body tier
#include "ssserial.h"   // <SS:Nexii> Strata - the version stamp digest, shared with every other Soapstorm store

// Cache organization:
// cache/texture.entries
//  Unordered array of Entry structs
// cache/texture.cache
//  First TEXTURE_CACHE_ENTRY_SIZE bytes of each texture in texture.entries in same order
// cache/textures/[0-F]/UUID.texture
//  Actual texture body files

//note: there is no good to define 1024 for TEXTURE_CACHE_ENTRY_SIZE while FIRST_PACKET_SIZE is 600 on sim side.
const S32 TEXTURE_CACHE_ENTRY_SIZE = FIRST_PACKET_SIZE;//1024;
const F32 TEXTURE_CACHE_PURGE_AMOUNT = .20f; // % amount to reduce the cache by when it exceeds its limit
const F32 TEXTURE_CACHE_LRU_SIZE = .10f; // % amount for LRU list (low overhead to regenerate)
const S32 TEXTURE_FAST_CACHE_ENTRY_OVERHEAD = sizeof(S32) * 4; //w, h, c, level
const S32 TEXTURE_FAST_CACHE_DATA_SIZE = 16 * 16 * 4;
const S32 TEXTURE_FAST_CACHE_ENTRY_SIZE = TEXTURE_FAST_CACHE_DATA_SIZE + TEXTURE_FAST_CACHE_ENTRY_OVERHEAD;
const F32 TEXTURE_LAZY_PURGE_TIME_LIMIT = .004f; // 4ms. Would be better to autoadjust, but there is a major cache rework in progress.
const F32 TEXTURE_PRUNING_MAX_TIME = 15.f;

class LLTextureCacheWorker : public LLWorkerClass
{
    friend class LLTextureCache;

private:
    class ReadResponder : public LLLFSThread::Responder
    {
    public:
        ReadResponder(LLTextureCache* cache, handle_t handle) : mCache(cache), mHandle(handle) {}
        ~ReadResponder() {}
        void completed(S32 bytes)
        {
            mCache->lockWorkers();
            LLTextureCacheWorker* reader = mCache->getReader(mHandle);
            if (reader) reader->ioComplete(bytes);
            mCache->unlockWorkers();
        }
        LLTextureCache* mCache;
        LLTextureCacheWorker::handle_t mHandle;
    };

    class WriteResponder : public LLLFSThread::Responder
    {
    public:
        WriteResponder(LLTextureCache* cache, handle_t handle) : mCache(cache), mHandle(handle) {}
        ~WriteResponder() {}
        void completed(S32 bytes)
        {
            mCache->lockWorkers();
            LLTextureCacheWorker* writer = mCache->getWriter(mHandle);
            if (writer) writer->ioComplete(bytes);
            mCache->unlockWorkers();
        }
        LLTextureCache* mCache;
        LLTextureCacheWorker::handle_t mHandle;
    };

public:
    LLTextureCacheWorker(LLTextureCache* cache, const LLUUID& id,
                         const U8* data, S32 datasize, S32 offset,
                         S32 imagesize, // for writes
                         LLTextureCache::Responder* responder)
        : LLWorkerClass(cache, "LLTextureCacheWorker"),
          mID(id),
          mCache(cache),
          mReadData(NULL),
          mWriteData(data),
          mDataSize(datasize),
          mOffset(offset),
          mImageSize(imagesize),
          mImageFormat(IMG_CODEC_J2C),
          mImageLocal(false),
          mResponder(responder),
          mFileHandle(LLLFSThread::nullHandle()),
          mBytesToRead(0),
          mBytesRead(0)
    {
    }
    ~LLTextureCacheWorker()
    {
        llassert_always(!haveWork());
        ll_aligned_free_16(mReadData);
    }

    // override this interface
    virtual bool doRead() = 0;
    virtual bool doWrite() = 0;

    virtual bool doWork(S32 param); // Called from LLWorkerThread::processRequest()

    handle_t read() { addWork(0); return mRequestHandle; }
    handle_t write() { addWork(1); return mRequestHandle; }
    bool complete() { return checkWork(); }
    void ioComplete(S32 bytes)
    {
        mBytesRead = bytes;
    }

private:
    virtual void startWork(S32 param); // called from addWork() (MAIN THREAD)
    virtual void finishWork(S32 param, bool completed); // called from finishRequest() (WORK THREAD)
    virtual void endWork(S32 param, bool aborted); // called from doWork() (MAIN THREAD)

protected:
    LLTextureCache* mCache;
    LLUUID  mID;

    U8* mReadData;
    const U8* mWriteData;
    S32 mDataSize;
    S32 mOffset;
    S32 mImageSize;
    EImageCodec mImageFormat;
    bool mImageLocal;
    LLPointer<LLTextureCache::Responder> mResponder;
    LLLFSThread::handle_t mFileHandle;
    S32 mBytesToRead;
    LLAtomicS32 mBytesRead;
};

class LLTextureCacheLocalFileWorker : public LLTextureCacheWorker
{
public:
    LLTextureCacheLocalFileWorker(LLTextureCache* cache, const std::string& filename, const LLUUID& id,
                         U8* data, S32 datasize, S32 offset,
                         S32 imagesize, // for writes
                         LLTextureCache::Responder* responder)
            : LLTextureCacheWorker(cache, id, data, datasize, offset, imagesize, responder),
            mFileName(filename)

    {
    }

    virtual bool doRead();
    virtual bool doWrite();

private:
    std::string mFileName;
};

bool LLTextureCacheLocalFileWorker::doRead()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    S32 local_size = LLAPRFile::size(mFileName, mCache->getLocalAPRFilePool());

    if (local_size > 0 && mFileName.size() > 4)
    {
        mDataSize = local_size; // Only a complete file is valid

        std::string extension = mFileName.substr(mFileName.size() - 3, 3);

        mImageFormat = LLImageBase::getCodecFromExtension(extension);

        if (mImageFormat == IMG_CODEC_INVALID)
        {
//          LL_WARNS() << "Unrecognized file extension " << extension << " for local texture " << mFileName << LL_ENDL;
            mDataSize = 0; // no data
            return true;
        }
    }
    else
    {
        // file doesn't exist
        mDataSize = 0; // no data
        return true;
    }

    if (!mDataSize || mDataSize > local_size)
    {
        mDataSize = local_size;
    }
    mReadData = (U8*)ll_aligned_malloc_16(mDataSize);

    S32 bytes_read = LLAPRFile::readEx(mFileName, mReadData, mOffset, mDataSize, mCache->getLocalAPRFilePool());

    if (bytes_read != mDataSize)
    {
//      LL_WARNS() << "Error reading file from local cache: " << mFileName
//              << " Bytes: " << mDataSize << " Offset: " << mOffset
//              << " / " << mDataSize << LL_ENDL;
        mDataSize = 0;
        ll_aligned_free_16(mReadData);
        mReadData = NULL;
    }
    else
    {
        mImageSize = local_size;
        mImageLocal = true;
    }
    return true;
}

bool LLTextureCacheLocalFileWorker::doWrite()
{
    // no writes for local files
    return false;
}

class LLTextureCacheRemoteWorker : public LLTextureCacheWorker
{
public:
    LLTextureCacheRemoteWorker(LLTextureCache* cache, const LLUUID& id,
                         const U8* data, S32 datasize, S32 offset,
                         S32 imagesize, // for writes
                         LLPointer<LLImageRaw> raw, S32 discardlevel,
                         LLTextureCache::Responder* responder)
            : LLTextureCacheWorker(cache, id, data, datasize, offset, imagesize, responder),
            mState(INIT),
            mRawImage(raw),
            mRawDiscardLevel(discardlevel)
    {
    }

    virtual bool doRead();
    virtual bool doWrite();

private:
    enum e_state
    {
        INIT = 0,
        LOCAL = 1,
        CACHE = 2,
        HEADER = 3,
        BODY = 4
    };

    e_state mState;
    LLPointer<LLImageRaw> mRawImage;
    S32 mRawDiscardLevel;
};


//virtual
void LLTextureCacheWorker::startWork(S32 param)
{
}

// This is where a texture is read from the cache system (header and body)
// Current assumption are:
// - the whole data are in a raw form, will be stored at mReadData
// - the size of this raw data is mDataSize and can be smaller than TEXTURE_CACHE_ENTRY_SIZE (the size of a record in the header cache)
// - the code supports offset reading but this is actually never exercised in the viewer
bool LLTextureCacheRemoteWorker::doRead()
{
    LL_PROFILE_ZONE_SCOPED;
    bool done = false;
    S32 idx = -1;

    S32 local_size = 0;
    std::string local_filename;

    // First state / stage : find out if the file is local
    if (mState == INIT)
    {
#if 0
        std::string filename = mCache->getLocalFileName(mID);
        // Is it a JPEG2000 file?
        {
            local_filename = filename + ".j2c";
            local_size = LLAPRFile::size(local_filename, mCache->getLocalAPRFilePool());
            if (local_size > 0)
            {
                mImageFormat = IMG_CODEC_J2C;
            }
        }
        // If not, is it a jpeg file?
        if (local_size == 0)
        {
            local_filename = filename + ".jpg";
            local_size = LLAPRFile::size(local_filename, mCache->getLocalAPRFilePool());
            if (local_size > 0)
            {
                mImageFormat = IMG_CODEC_JPEG;
                mDataSize = local_size; // Only a complete .jpg file is valid
            }
        }
        // Hmm... What about a targa file? (used for UI texture mostly)
        if (local_size == 0)
        {
            local_filename = filename + ".tga";
            local_size = LLAPRFile::size(local_filename, mCache->getLocalAPRFilePool());
            if (local_size > 0)
            {
                mImageFormat = IMG_CODEC_TGA;
                mDataSize = local_size; // Only a complete .tga file is valid
            }
        }
        // Determine the next stage: if we found a file, then LOCAL else CACHE
        mState = (local_size > 0 ? LOCAL : CACHE);

        llassert_always(mState == CACHE) ;
#else
        mState = CACHE;
#endif
    }

    // Second state / stage : if the file is local, load it and leave
    if (!done && (mState == LOCAL))
    {
        llassert(local_size != 0);  // we're assuming there is a non empty local file here...
        if (!mDataSize || mDataSize > local_size)
        {
            mDataSize = local_size;
        }
        // Allocate read buffer
        mReadData = (U8*)ll_aligned_malloc_16(mDataSize);

        if (mReadData)
        {
            S32 bytes_read = LLAPRFile::readEx( local_filename,
                                                mReadData,
                                                mOffset,
                                                mDataSize,
                                                mCache->getLocalAPRFilePool());

            if (bytes_read != mDataSize)
            {
                LL_WARNS() << "Error reading file from local cache: " << local_filename
                        << " Bytes: " << mDataSize << " Offset: " << mOffset
                    << " / " << mDataSize << LL_ENDL;
                mDataSize = 0;
                ll_aligned_free_16(mReadData);
                mReadData = NULL;
            }
            else
            {
                mImageSize = local_size;
                mImageLocal = true;
            }
        }
        else
        {
            LL_WARNS() << "Error allocating memory for cache: " << local_filename
                    << " of size: " << mDataSize << LL_ENDL;
            mDataSize = 0;
        }
        // We're done...
        done = true;
    }

    // Second state / stage : identify the cache or not...
    if (!done && (mState == CACHE))
    {
        LLTextureCache::Entry entry ;
        idx = mCache->getHeaderCacheEntry(mID, entry);
        if (idx < 0)
        {
            // The texture is *not* cached. We're done here...
            mDataSize = 0; // no data
            done = true;
        }
        else
        {
            mImageSize = entry.mImageSize ;
            // If the read offset is bigger than the header cache, we read directly from the body
            // Note that currently, we *never* read with offset from the cache, so the result is *always* HEADER
            mState = mOffset < TEXTURE_CACHE_ENTRY_SIZE ? HEADER : BODY;
        }
    }

    // Third state / stage : read data from the header cache (texture.entries) file
    if (!done && (mState == HEADER))
    {
        llassert_always(idx >= 0);  // we need an entry here or reading the header makes no sense
        llassert_always(mOffset < TEXTURE_CACHE_ENTRY_SIZE);
        S32 offset = idx * TEXTURE_CACHE_ENTRY_SIZE + mOffset;
        // Compute the size we need to read (in bytes)
        S32 size = TEXTURE_CACHE_ENTRY_SIZE - mOffset;
        size = llmin(size, mDataSize);
        // Allocate the read buffer
        mReadData = (U8*)ll_aligned_malloc_16(size);
        if (mReadData)
        {
            S32 bytes_read = LLAPRFile::readEx(mCache->mHeaderDataFileName,
                                                 mReadData, offset, size, mCache->getLocalAPRFilePool());
            if (bytes_read != size)
            {
                LL_WARNS() << "LLTextureCacheWorker: "  << mID
                        << " incorrect number of bytes read from header: " << bytes_read
                        << " / " << size << LL_ENDL;
                ll_aligned_free_16(mReadData);
                mReadData = NULL;
                mDataSize = -1; // failed
                done = true;
            }
            // If we already read all we expected, we're actually done
            if (mDataSize <= bytes_read)
            {
                done = true;
            }
            else
            {
                mState = BODY;
            }
        }
        else
        {
            LL_WARNS() << "LLTextureCacheWorker: "  << mID
                << " failed to allocate memory for reading: " << mDataSize << LL_ENDL;
            mReadData = NULL;
            mDataSize = -1; // failed
            done = true;
        }
    }

    // Fourth state / stage : read the rest of the data from the UUID based cached file
    if (!done && (mState == BODY))
    {
        std::string filename = mCache->getTextureFileName(mID);
        // <SS:Nexii> Strata - the body may live inside a volume rather than in a file of its own, and the size has to come from the same place the bytes will. Asking the filesystem first would report zero for a packed body and this branch would then quietly decide the texture has no body at all, serving 600 bytes and calling it a whole image.
        S32 filesize = mCache->bodySize(mID, filename, mCache->getLocalAPRFilePool());
        // </SS:Nexii>

        if (filesize && (filesize + TEXTURE_CACHE_ENTRY_SIZE) > mOffset)
        {
            S32 max_datasize = TEXTURE_CACHE_ENTRY_SIZE + filesize - mOffset;
            mDataSize = llmin(max_datasize, mDataSize);

            S32 data_offset, file_size, file_offset;

            // Reserve the whole data buffer first
            U8* data = (U8*)ll_aligned_malloc_16(mDataSize);
            if (data)
            {
                // Set the data file pointers taking the read offset into account. 2 cases:
                if (mOffset < TEXTURE_CACHE_ENTRY_SIZE)
                {
                    // Offset within the header record. That means we read something from the header cache.
                    // Note: most common case is (mOffset = 0), so this is the "normal" code path.
                    data_offset = TEXTURE_CACHE_ENTRY_SIZE - mOffset;   // i.e. TEXTURE_CACHE_ENTRY_SIZE if mOffset nul (common case)
                    file_offset = 0;
                    file_size = mDataSize - data_offset;
                    // Copy the raw data we've been holding from the header cache into the new sized buffer
                    llassert_always(mReadData);
                    memcpy(data, mReadData, data_offset);
                    ll_aligned_free_16(mReadData);
                    mReadData = NULL;
                }
                else
                {
                    // Offset bigger than the header record. That means we haven't read anything yet.
                    data_offset = 0;
                    file_offset = mOffset - TEXTURE_CACHE_ENTRY_SIZE;
                    file_size = mDataSize;
                    // No data from header cache to copy in that case, we skipped it all
                }

                // Now use that buffer as the object read buffer
                llassert_always(mReadData == NULL);
                mReadData = data;

                // Read the data at last
                // <SS:Nexii> Strata - one call for both tiers. readBody serves a packed body straight out of its volume with the same one open, one seek, one read the loose path costs, and falls through to exactly the LLAPRFile::readEx below when the object was never packed.
                S32 bytes_read = mCache->readBody(mID, filename, file_offset, mReadData + data_offset, file_size, mCache->getLocalAPRFilePool());
                // </SS:Nexii>
                if (bytes_read != file_size)
                {
                    LL_WARNS() << "LLTextureCacheWorker: "  << mID
                            << " incorrect number of bytes read from body: " << bytes_read
                            << " / " << file_size << LL_ENDL;
                    ll_aligned_free_16(mReadData);
                    mReadData = NULL;
                    mDataSize = -1; // failed
                    done = true;
                }
            }
            else
            {
                LL_WARNS() << "LLTextureCacheWorker: "  << mID
                    << " failed to allocate memory for reading: " << mDataSize << LL_ENDL;
                ll_aligned_free_16(mReadData);
                mReadData = NULL;
                mDataSize = -1; // failed
                done = true;
            }
        }
        else
        {
            // No body, we're done.
            mDataSize = llmax(TEXTURE_CACHE_ENTRY_SIZE - mOffset, 0);
            LL_DEBUGS() << "No body file for: " << filename << LL_ENDL;
        }
        // Nothing else to do at that point...
        done = true;
    }

    // Clean up and exit
    return done;
}

// This is where *everything* about a texture is written down in the cache system (entry map, header and body)
// Current assumption are:
// - the whole data are in a raw form, starting at mWriteData
// - the size of this raw data is mDataSize and can be smaller than TEXTURE_CACHE_ENTRY_SIZE (the size of a record in the header cache)
// - the code *does not* support offset writing so there are no difference between buffer addresses and start of data
bool LLTextureCacheRemoteWorker::doWrite()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    bool done = false;
    S32 idx = -1;

    // First state / stage : check that what we're trying to cache is in an OK shape
    if (mState == INIT)
    {
        if ((mOffset != 0) // We currently do not support write offsets
            || (mDataSize <= 0) // Things will go badly wrong if mDataSize is nul or negative...
            || (mImageSize < mDataSize)
            || (mRawDiscardLevel < 0)
            || (mRawImage->isBufferInvalid())) // decode failed or malfunctioned, don't write
        {
            LL_WARNS() << "INIT state check failed for image: " << mID << " Size: " << mImageSize << " DataSize: " << mDataSize << " Discard:" << mRawDiscardLevel << LL_ENDL;
            mDataSize = -1; // failed
            done = true;
        }
        else
        {
            mState = CACHE;
        }
    }

    // No LOCAL state for write(): because it doesn't make much sense to cache a local file...

    // Second state / stage : set an entry in the headers entry (texture.entries) file
    if (!done && (mState == CACHE))
    {
        bool alreadyCached = false;
        LLTextureCache::Entry entry;

        // Checks if this image is already in the entry list
        idx = mCache->getHeaderCacheEntry(mID, entry);
        if(idx < 0)
        {
            idx = mCache->setHeaderCacheEntry(mID, entry, mImageSize, mDataSize); // create the new entry.
            if(idx >= 0)
            {
                // write to the fast cache.
                // mRawImage is not entirely safe here since it is a pointer to one owned by cache worker,
                // it could have been retrieved via getRequestFinished() and then modified.
                // If writeToFastCache crashes, something is wrong around fetch worker.
                if(!mCache->writeToFastCache(mID, idx, mRawImage, mRawDiscardLevel))
                {
                    LL_WARNS() << "writeToFastCache failed" << LL_ENDL;
                    mDataSize = -1; // failed
                    done = true;
                }
            }
        }
        else
        {
            alreadyCached = mCache->updateEntry(idx, entry, mImageSize, mDataSize); // update the existing entry.
        }

        if (!done)
        {
            if (idx < 0)
            {
                LL_WARNS() << "LLTextureCacheWorker: " << mID
                    << " Unable to create header entry for writing!" << LL_ENDL;
                mDataSize = -1; // failed
                done = true;
            }
            else
            {
                if (alreadyCached && (mDataSize <= TEXTURE_CACHE_ENTRY_SIZE))
                {
                    // Small texture already cached case: we're done with writing
                    done = true;
                }
                else
                {
                    // If the texture has already been cached, we don't resave the header and go directly to the body part
                    mState = alreadyCached ? BODY : HEADER;
                }
            }
        }
    }


    // Third stage / state : write the header record in the header file (texture.cache)
    if (!done && (mState == HEADER))
    {
        if (idx < 0) // we need an entry here or storing the header makes no sense
        {
            LL_WARNS() << "index check failed" << LL_ENDL;
            mDataSize = -1; // failed
            done = true;
        }
        else
        {
            S32 offset = idx * TEXTURE_CACHE_ENTRY_SIZE;    // skip to the correct spot in the header file
            S32 size = TEXTURE_CACHE_ENTRY_SIZE;            // record size is fixed for the header
            S32 bytes_written;

            if (mDataSize < TEXTURE_CACHE_ENTRY_SIZE)
            {
                // We need to write a full record in the header cache so, if the amount of data is smaller
                // than a record, we need to transfer the data to a buffer padded with 0 and write that
                U8* padBuffer = (U8*)ll_aligned_malloc_16(TEXTURE_CACHE_ENTRY_SIZE);
                memset(padBuffer, 0, TEXTURE_CACHE_ENTRY_SIZE);     // Init with zeros
                memcpy(padBuffer, mWriteData, mDataSize);           // Copy the write buffer
                bytes_written = LLAPRFile::writeEx(mCache->mHeaderDataFileName, padBuffer, offset, size, mCache->getLocalAPRFilePool());
                ll_aligned_free_16(padBuffer);
            }
            else
            {
                // Write the header record (== first TEXTURE_CACHE_ENTRY_SIZE bytes of the raw file) in the header file
                bytes_written = LLAPRFile::writeEx(mCache->mHeaderDataFileName, mWriteData, offset, size, mCache->getLocalAPRFilePool());
            }

            if (bytes_written <= 0)
            {
                LL_WARNS() << "LLTextureCacheWorker: " << mID
                    << " Unable to write header entry!" << LL_ENDL;
                mDataSize = -1; // failed
                done = true;
            }

            // If we wrote everything (may be more with padding) in the header cache,
            // we're done so we don't have a body to store
            if (mDataSize <= bytes_written)
            {
                done = true;
            }
            else
            {
                mState = BODY;
            }
        }
    }

    // Fourth stage / state : write the body file, i.e. the rest of the texture in a "UUID" file name
    if (!done && (mState == BODY))
    {
        if (mDataSize <= TEXTURE_CACHE_ENTRY_SIZE) // wouldn't make sense to be here otherwise...
        {
            LL_WARNS() << "mDataSize check failed" << LL_ENDL;
            mDataSize = -1; // failed
            done = true;
        }
        else
        {
            S32 file_size = mDataSize - TEXTURE_CACHE_ENTRY_SIZE;

            {
                // build the cache file name from the UUID
                std::string filename = mCache->getTextureFileName(mID);
                //          LL_INFOS() << "Writing Body: " << filename << " Bytes: " << file_offset+file_size << LL_ENDL;
                // <SS:Nexii> Strata - writeBody revokes any packed record for this uuid BEFORE the loose file is written, which is what stops a stale volume copy shadowing the newer body a better discard level just produced. The write itself is the same syscall on the same thread as before; nothing here waits on container IO.
                S32 bytes_written = mCache->writeBody(mID, filename, mWriteData + TEXTURE_CACHE_ENTRY_SIZE, file_size) ? file_size : 0;
                // </SS:Nexii>
                if (bytes_written <= 0)
                {
                    LL_WARNS() << "LLTextureCacheWorker: " << mID
                        << " incorrect number of bytes written to body: " << bytes_written
                        << " / " << file_size << LL_ENDL;
                    mDataSize = -1; // failed
                    done = true;
                }
            }

            // Nothing else to do at that point...
            done = true;
        }
    }
    mRawImage = NULL;

    // Clean up and exit
    return done;
}

//virtual
bool LLTextureCacheWorker::doWork(S32 param)
{
    LL_PROFILE_ZONE_SCOPED;
    bool res = false;
    if (param == 0) // read
    {
        res = doRead();
    }
    else if (param == 1) // write
    {
        res = doWrite();
    }
    else
    {
        llassert_always(0);
    }
    return res;
}

//virtual (WORKER THREAD)
void LLTextureCacheWorker::finishWork(S32 param, bool completed)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (mResponder.notNull())
    {
        bool success = (completed && mDataSize > 0);
        if (param == 0)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("tcwfw - read");
            // read
            if (success)
            {
                mResponder->setData(mReadData, mDataSize, mImageSize, mImageFormat, mImageLocal);
                mReadData = NULL; // responder owns data
                mDataSize = 0;
            }
            else
            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("tcwfw - read fail");
                ll_aligned_free_16(mReadData);
                mReadData = NULL;
            }
        }
        else
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("tcwfw - write");
            // write
            mWriteData = NULL; // we never owned data
            mDataSize = 0;
        }
        mCache->addCompleted(mResponder, success);
    }
}

//virtual (MAIN THREAD)
void LLTextureCacheWorker::endWork(S32 param, bool aborted)
{
    LL_PROFILE_ZONE_SCOPED;
    if (aborted)
    {
        // Let the destructor handle any cleanup
        return;
    }
    switch(param)
    {
      default:
      case 0: // read
      case 1: // write
      {
          if (mDataSize < 0)
          {
              // failed
              mCache->removeFromCache(mID);
          }
          break;
      }
    }
}

//////////////////////////////////////////////////////////////////////////////

LLTextureCache::LLTextureCache(bool threaded)
    : LLWorkerThread("TextureCache", threaded),
      mWorkersMutex(),
      mHeaderMutex(),
      mListMutex(),
      mFastCacheMutex(),
      mHeaderAPRFile(NULL),
      mReadOnly(true), //do not allow to change the texture cache until setReadOnly() is called.
      mTexturesSizeTotal(0),
      mDoPurge(false),
      mFastCachep(NULL),
      mFastCachePoolp(NULL),
      mFastCachePadBuffer(NULL)
{
    mHeaderAPRFilePoolp = new LLVolatileAPRPool(); // is_local = true, because this pool is for headers, headers are under own mutex
}

LLTextureCache::~LLTextureCache()
{
    clearDeleteList() ;
    writeUpdatedEntries() ;
    delete mFastCachep;
    delete mFastCachePoolp;
    delete mHeaderAPRFilePoolp;
    ll_aligned_free_16(mFastCachePadBuffer);
}

//////////////////////////////////////////////////////////////////////////////

//virtual
size_t LLTextureCache::update(F32 max_time_ms)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    static LLFrameTimer timer ;
    static const F32 MAX_TIME_INTERVAL = 300.f ; //seconds.

    size_t res;
    res = LLWorkerThread::update(max_time_ms);

    mListMutex.lock();
    handle_list_t priorty_list = mPrioritizeWriteList; // copy list
    mPrioritizeWriteList.clear();
    responder_list_t completed_list = mCompletedList; // copy list
    mCompletedList.clear();
    mListMutex.unlock();

    // call 'completed' with workers list unlocked (may call readComplete() or writeComplete()
    for (responder_list_t::iterator iter1 = completed_list.begin();
         iter1 != completed_list.end(); ++iter1)
    {
        Responder *responder = iter1->first;
        bool success = iter1->second;
        responder->completed(success);
    }

    if(!res && timer.getElapsedTimeF32() > MAX_TIME_INTERVAL)
    {
        timer.reset() ;
        writeUpdatedEntries() ;
    }

    return res;
}

//////////////////////////////////////////////////////////////////////////////
// search for local copy of UUID-based image file
std::string LLTextureCache::getLocalFileName(const LLUUID& id)
{
    // Does not include extension
    std::string idstr = id.asString();
    // TODO: should we be storing cached textures in skin directory?
    std::string filename = gDirUtilp->getExpandedFilename(LL_PATH_LOCAL_ASSETS, idstr);
    return filename;
}

std::string LLTextureCache::getTextureFileName(const LLUUID& id)
{
    std::string idstr = id.asString();
    std::string delem = gDirUtilp->getDirDelimiter();
    std::string filename = mTexturesDirName + delem + idstr[0] + delem + idstr + ".texture";
    return filename;
}

//debug
bool LLTextureCache::isInCache(const LLUUID& id)
{
    LLMutexLock lock(&mHeaderIDMapMutex);
    id_map_t::const_iterator iter = mHeaderIDMap.find(id);

    return (iter != mHeaderIDMap.end()) ;
}

//debug
bool LLTextureCache::isInLocal(const LLUUID& id)
{
    S32 local_size = 0;
    std::string local_filename;

    std::string filename = getLocalFileName(id);
    // Is it a JPEG2000 file?
    {
        local_filename = filename + ".j2c";
        local_size = LLAPRFile::size(local_filename, getLocalAPRFilePool());
        if (local_size > 0)
        {
            return true ;
        }
    }

    // If not, is it a jpeg file?
    {
        local_filename = filename + ".jpg";
        local_size = LLAPRFile::size(local_filename, getLocalAPRFilePool());
        if (local_size > 0)
        {
            return true ;
        }
    }

    // Hmm... What about a targa file? (used for UI texture mostly)
    {
        local_filename = filename + ".tga";
        local_size = LLAPRFile::size(local_filename, getLocalAPRFilePool());
        if (local_size > 0)
        {
            return true ;
        }
    }

    return false ;
}
//////////////////////////////////////////////////////////////////////////////

//static
F32 LLTextureCache::sHeaderCacheVersion = 1.71f;
U32 LLTextureCache::sCacheMaxEntries = 1024 * 1024; //~1 million textures.
std::atomic<S64> LLTextureCache::sCacheMaxTexturesSize{0}; // <SS:Nexii/> no limit; atomic because the budget arbiter moves it mid-session, see LLTextureCache::ssSetTexturesBudget
std::string LLTextureCache::sHeaderCacheEncoderVersion = LLImageJ2C::getEngineInfo();

#if defined(ADDRESS_SIZE)
U32 LLTextureCache::sHeaderCacheAddressSize = ADDRESS_SIZE;
#else
U32 LLTextureCache::sHeaderCacheAddressSize = 32;
#endif

const char* entries_filename = "texture.entries";
const char* cache_filename = "texture.cache";
const char* old_textures_dirname = "textures";
//change the location of the texture cache to prevent from being deleted by old version viewers.
const char* textures_dirname = "texturecache";
const char* fast_cache_filename = "FastCache.cache";

void LLTextureCache::setDirNames(ELLPath location)
{
    std::string delem = gDirUtilp->getDirDelimiter();

    mCacheParentDirName = gDirUtilp->getExpandedFilename(location,"");
    mHeaderEntriesFileName = gDirUtilp->getExpandedFilename(location, textures_dirname, entries_filename);
    mHeaderDataFileName = gDirUtilp->getExpandedFilename(location, textures_dirname, cache_filename);
    mTexturesDirName = gDirUtilp->getExpandedFilename(location, textures_dirname);
    mFastCacheFileName =  gDirUtilp->getExpandedFilename(location, textures_dirname, fast_cache_filename);
}

void LLTextureCache::purgeCache(ELLPath location, bool remove_dir)
{
    LLMutexLock lock(&mHeaderMutex);

    if (!mReadOnly)
    {
        setDirNames(location);
        llassert_always(mHeaderAPRFile == NULL);

        //remove the legacy cache if exists
        std::string texture_dir = mTexturesDirName ;
        mTexturesDirName = gDirUtilp->getExpandedFilename(location, old_textures_dirname);
        if(LLFile::isdir(mTexturesDirName))
        {
            std::string file_name = gDirUtilp->getExpandedFilename(location, entries_filename);
            // mHeaderAPRFilePoolp because we are under header mutex, and can be in main thread
            LLAPRFile::remove(file_name, mHeaderAPRFilePoolp);

            file_name = gDirUtilp->getExpandedFilename(location, cache_filename);
            LLAPRFile::remove(file_name, mHeaderAPRFilePoolp);

            purgeAllTextures(true);
        }
        mTexturesDirName = texture_dir ;
    }

    //remove the current texture cache.
    purgeAllTextures(remove_dir);
}

//is called in the main thread before initCache(...) is called.
void LLTextureCache::setReadOnly(bool read_only)
{
    mReadOnly = read_only ;
}

// Called in the main thread.
// Returns the unused amount of max_size if any
S64 LLTextureCache::initCache(ELLPath location, S64 max_size, bool texture_cache_mismatch)
{
    LL_PROFILE_ZONE_SCOPED;

    llassert_always(getPending() == 0) ; //should not start accessing the texture cache before initialized.

    S64 entries_size = (max_size * 36) / 100; //0.36 * max_size
    S64 max_entries = entries_size / (TEXTURE_CACHE_ENTRY_SIZE + TEXTURE_FAST_CACHE_ENTRY_SIZE);
    sCacheMaxEntries = (S32)(llmin((S64)sCacheMaxEntries, max_entries));
    entries_size = sCacheMaxEntries * (TEXTURE_CACHE_ENTRY_SIZE + TEXTURE_FAST_CACHE_ENTRY_SIZE);
    max_size -= entries_size;
    // <SS:Nexii> Unchanged in behaviour - only spelled through the atomic's load and store, because the arbiter can move this number from the main thread while a purge pass is reading it. See ssSetTexturesBudget below and doc/strata.md.
    if (sCacheMaxTexturesSize.load() > 0)
        sCacheMaxTexturesSize.store(llmin(sCacheMaxTexturesSize.load(), max_size));
    else
        sCacheMaxTexturesSize.store(max_size);
    max_size -= sCacheMaxTexturesSize.load();
    // </SS:Nexii>

    LL_INFOS("TextureCache") << "Headers: " << sCacheMaxEntries
            << " Textures size: " << sCacheMaxTexturesSize.load() / (1024 * 1024) << " MB" << LL_ENDL;

    setDirNames(location);

    if(texture_cache_mismatch)
    {
        //if readonly, disable the texture cache,
        //otherwise wipe out the texture cache.
        purgeAllTextures(true);

        if(mReadOnly)
        {
            return max_size ;
        }
    }

    if (!mReadOnly)
    {
            if (!LLFile::isdir(mCacheParentDirName))
            {
                    LLFile::mkdir(mCacheParentDirName);
                }
        LLFile::mkdir(mTexturesDirName);

        const char* subdirs = "0123456789abcdef";
        for (S32 i=0; i<16; i++)
        {
            std::string dirname = mTexturesDirName + gDirUtilp->getDirDelimiter() + subdirs[i];
            LLFile::mkdir(dirname);
        }
    }
    // <SS:Nexii> Strata - the J2C body tier, brought up BEFORE readHeaderCache because readHeaderCache can decide the header is stale and call purgeAllTextures, which wipes the volumes; a store that was not yet able to name its own directory could not be wiped by it. It is also before purgeTextures, whose validation pass asks bodySize() whether a body still exists.
    //
    // The version stamp is a digest of every number that already invalidates this cache - the header cache version, the pointer size and the J2C encoder string - rather than DiskCacheVersion, which governs a different tier entirely. Any change that makes readHeaderCache purge also changes this digest, so the volumes are orphaned by the same event that orphans the entries, belt and braces.
    {
        std::vector<U8> stamp;
        ssserial::Writer w(stamp);
        w.pod<F32>(sHeaderCacheVersion);
        w.pod<U32>(sHeaderCacheAddressSize);
        w.bytes(sHeaderCacheEncoderVersion.c_str(), sHeaderCacheEncoderVersion.size());

        SSStrataStore::Config cfg;
        cfg.mEnabled        = gSavedSettings.getBOOL("SSStrataTextures");
        cfg.mReadOnly       = mReadOnly;   // a second instance shares this directory and never writes into it, exactly as it never evicts from it
        cfg.mVerbose        = gSavedSettings.getBOOL("EnableDiskCacheDebugInfo");
        cfg.mPackAgeSecs    = gSavedSettings.getU32("SSStrataPackAgeSeconds");
        cfg.mMaxObjectBytes = SSSTRATA_TEX_MAX_OBJECT_BYTES;
        cfg.mDefaultAssetType = (U8)LLAssetType::AT_TEXTURE;   // every object in this tenant is a J2C body, so the type is a constant rather than something to remember per uuid
        cfg.mDiskCacheVersion = ssserial::crc32Of(stamp.data(), stamp.size());

        SSStrataStore& tex = SSStrataStore::tier(SSSTRATA_TENANT_TEXTURES);
        tex.configure(cfg);
        tex.initStore(mTexturesDirName, (U64)llmax((S64)0, sCacheMaxTexturesSize.load()));
    }
    // </SS:Nexii>

    readHeaderCache();
    purgeTextures(true); // calc mTexturesSize and make some room in the texture cache if we need it

    llassert_always(getPending() == 0) ; //should not start accessing the texture cache before initialized.
    openFastCache(true);

    return max_size; // unused cache space
}

//----------------------------------------------------------------------------
// mHeaderMutex must be locked for the following functions!

LLAPRFile* LLTextureCache::openHeaderEntriesFile(bool readonly, S32 offset)
{
    llassert_always(mHeaderAPRFile == NULL);
    apr_int32_t flags = readonly ? APR_READ|APR_BINARY : APR_READ|APR_WRITE|APR_BINARY;
    mHeaderAPRFile = new LLAPRFile(mHeaderEntriesFileName, flags, mHeaderAPRFilePoolp);
    if(offset > 0)
    {
        mHeaderAPRFile->seek(APR_SET, offset);
    }
    return mHeaderAPRFile;
}

void LLTextureCache::closeHeaderEntriesFile()
{
    if(!mHeaderAPRFile)
    {
        return ;
    }

    delete mHeaderAPRFile;
    mHeaderAPRFile = NULL;
}

void LLTextureCache::readEntriesHeader()
{
    // mHeaderEntriesInfo initializes to default values so safe not to read it
    llassert_always(mHeaderAPRFile == NULL);
    if (LLAPRFile::isExist(mHeaderEntriesFileName, mHeaderAPRFilePoolp))
    {
        LLAPRFile::readEx(mHeaderEntriesFileName, (U8*)&mHeaderEntriesInfo, 0, sizeof(EntriesInfo),
                          mHeaderAPRFilePoolp);
    }
    else //create an empty entries header.
    {
        setEntriesHeader();
        writeEntriesHeader() ;
    }
}

void LLTextureCache::setEntriesHeader()
{
    if (sHeaderEncoderStringSize < sHeaderCacheEncoderVersion.size() + 1)
    {
        // For simplicity we use predefined size of header, so if version string
        // doesn't fit, either getEngineInfo() returned malformed string or
        // sHeaderEncoderStringSize need to be increased.
        // Also take into accout that c_str() returns additional null character
        LL_ERRS() << "Version string doesn't fit in header" << LL_ENDL;
    }

    mHeaderEntriesInfo.mVersion = sHeaderCacheVersion;
    mHeaderEntriesInfo.mAdressSize = sHeaderCacheAddressSize;
    strcpy(mHeaderEntriesInfo.mEncoderVersion, sHeaderCacheEncoderVersion.c_str());
    mHeaderEntriesInfo.mEntries = 0;
}

void LLTextureCache::writeEntriesHeader()
{
    llassert_always(mHeaderAPRFile == NULL);
    if (!mReadOnly)
    {
        LLAPRFile::writeEx(mHeaderEntriesFileName, (U8*)&mHeaderEntriesInfo, 0, sizeof(EntriesInfo),
                           mHeaderAPRFilePoolp);
    }
}

//mHeaderMutex is locked before calling this.
S32 LLTextureCache::openAndReadEntry(const LLUUID& id, Entry& entry, bool create)
{
    S32 idx = -1;

    {
        LLMutexLock lock(&mHeaderIDMapMutex);
        id_map_t::iterator iter1 = mHeaderIDMap.find(id);
        if (iter1 != mHeaderIDMap.end())
        {
            idx = iter1->second;
        }
    }

    if (idx < 0)
    {
        if (create && !mReadOnly)
        {
            if (mHeaderEntriesInfo.mEntries < sCacheMaxEntries)
            {
                // Add an entry to the end of the list
                idx = mHeaderEntriesInfo.mEntries++;

            }
            else if (!mFreeList.empty())
            {
                idx = *(mFreeList.begin());
                mFreeList.erase(mFreeList.begin());
            }
            else
            {
                // Look for a still valid entry in the LRU
                for (std::set<LLUUID>::iterator iter2 = mLRU.begin(); iter2 != mLRU.end();)
                {
                    std::set<LLUUID>::iterator curiter2 = iter2++;
                    LLUUID oldid = *curiter2;
                    // Erase entry from LRU regardless
                    mLRU.erase(curiter2);
                    // Look up entry and use it if it is valid

                    S32 found_idx = -1;
                    {
                        LLMutexLock lock(&mHeaderIDMapMutex);
                        id_map_t::iterator iter3 = mHeaderIDMap.find(oldid);
                        if (iter3 != mHeaderIDMap.end() && iter3->second >= 0)
                        {
                            found_idx = iter3->second;
                        }
                    }
                    if (found_idx >= 0)
                    {
                        idx = found_idx;
                        removeCachedTexture(oldid) ;//remove the existing cached texture to release the entry index.
                        break;
                    }
                }
                // if (idx < 0) at this point, we will rebuild the LRU
                //  and retry if called from setHeaderCacheEntry(),
                //  otherwise this shouldn't happen and will trigger an error
            }
            if (idx >= 0)
            {
                entry.mID = id ;
                entry.mImageSize = -1 ; //mark it is a brand-new entry.
                entry.mBodySize = 0 ;
            }
        }
    }
    else
    {
        // Remove this entry from the LRU if it exists
        mLRU.erase(id);
        // Read the entry
        idx_entry_map_t::iterator iter = mUpdatedEntryMap.find(idx) ;
        if(iter != mUpdatedEntryMap.end())
        {
            entry = iter->second ;
        }
        else
        {
            readEntryFromHeaderImmediately(idx, entry) ;
        }
        if(entry.mImageSize <= entry.mBodySize)//it happens on 64-bit systems, do not know why
        {
            LL_WARNS() << "corrupted entry: " << id << " entry image size: " << entry.mImageSize << " entry body size: " << entry.mBodySize << LL_ENDL ;

            //erase this entry and the cached texture from the cache.
            std::string tex_filename = getTextureFileName(id);
            removeEntry(idx, entry, tex_filename) ;
            mUpdatedEntryMap.erase(idx) ;
            idx = -1 ;
        }
    }
    return idx;
}

//mHeaderMutex is locked before calling this.
void LLTextureCache::writeEntryToHeaderImmediately(S32& idx, Entry& entry, bool write_header)
{
    LLAPRFile* aprfile ;
    S32 bytes_written ;
    S32 offset = sizeof(EntriesInfo) + idx * sizeof(Entry);
    if(write_header)
    {
        aprfile = openHeaderEntriesFile(false, 0);
        bytes_written = aprfile->write((U8*)&mHeaderEntriesInfo, sizeof(EntriesInfo)) ;
        if(bytes_written != sizeof(EntriesInfo))
        {
            clearCorruptedCache() ; //clear the cache.
            idx = -1 ;//mark the idx invalid.
            return ;
        }

        mHeaderAPRFile->seek(APR_SET, offset);
    }
    else
    {
        aprfile = openHeaderEntriesFile(false, offset);
    }
    bytes_written = aprfile->write((void*)&entry, (S32)sizeof(Entry));
    if(bytes_written != sizeof(Entry))
    {
        clearCorruptedCache() ; //clear the cache.
        idx = -1 ;//mark the idx invalid.

        return ;
    }

    closeHeaderEntriesFile();
    mUpdatedEntryMap.erase(idx) ;
}

//mHeaderMutex is locked before calling this.
void LLTextureCache::readEntryFromHeaderImmediately(S32& idx, Entry& entry)
{
    S32 offset = sizeof(EntriesInfo) + idx * sizeof(Entry);
    LLAPRFile* aprfile = openHeaderEntriesFile(true, offset);
    S32 bytes_read = aprfile->read((void*)&entry, (S32)sizeof(Entry));
    closeHeaderEntriesFile();

    if(bytes_read != sizeof(Entry))
    {
        clearCorruptedCache() ; //clear the cache.
        idx = -1 ;//mark the idx invalid.
    }
}

//mHeaderMutex is locked before calling this.
//update an existing entry time stamp, delay writing.
void LLTextureCache::updateEntryTimeStamp(S32 idx, Entry& entry)
{
    static const U32 MAX_ENTRIES_WITHOUT_TIME_STAMP = (U32)(LLTextureCache::sCacheMaxEntries * 0.75f) ;

    if(mHeaderEntriesInfo.mEntries < MAX_ENTRIES_WITHOUT_TIME_STAMP)
    {
        return ; //there are enough empty entry index space, no need to stamp time.
    }

    if (idx >= 0)
    {
        if (!mReadOnly)
        {
            entry.mTime = (U32)time(NULL);
            mUpdatedEntryMap[idx] = entry ;
        }
    }
}

//update an existing entry, write to header file immediately.
bool LLTextureCache::updateEntry(S32& idx, Entry& entry, S32 new_image_size, S32 new_data_size)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    S32 new_body_size = llmax(0, new_data_size - TEXTURE_CACHE_ENTRY_SIZE) ;

    if(new_image_size == entry.mImageSize && new_body_size == entry.mBodySize)
    {
        return true ; //nothing changed.
    }
    else
    {
        bool purge = false ;

        lockHeaders() ;

        bool update_header = false ;
        if(entry.mImageSize < 0) //is a brand-new entry
        {
            {
                LLMutexLock lock(&mHeaderIDMapMutex);
                mHeaderIDMap[entry.mID] = idx;
            }
            mTexturesSizeMap[entry.mID] = new_body_size ;
            mTexturesSizeTotal += new_body_size ;

            // Update Header
            update_header = true ;
        }
        else if (entry.mBodySize != new_body_size)
        {
            //already in mHeaderIDMap.
            mTexturesSizeMap[entry.mID] = new_body_size ;
            mTexturesSizeTotal -= entry.mBodySize ;
            mTexturesSizeTotal += new_body_size ;
        }
        entry.mTime = (U32)time(NULL);
        entry.mImageSize = new_image_size ;
        entry.mBodySize = new_body_size ;

        writeEntryToHeaderImmediately(idx, entry, update_header) ;

        if (mTexturesSizeTotal > sCacheMaxTexturesSize.load())   // <SS:Nexii/> atomic load only; the comparison and its consequence are unchanged
        {
            purge = true;
        }

        unlockHeaders() ;

        if (purge)
        {
            mDoPurge = true;
        }
    }

    return false ;
}

U32 LLTextureCache::openAndReadEntries(std::vector<Entry>& entries)
{
    LLMutexLock lock(&mHeaderIDMapMutex);
    U32 num_entries = mHeaderEntriesInfo.mEntries;
    mHeaderIDMap.clear();
    mTexturesSizeMap.clear();
    mFreeList.clear();
    mTexturesSizeTotal = 0;

    LLAPRFile* aprfile = NULL;
    if(mUpdatedEntryMap.empty())
    {
        aprfile = openHeaderEntriesFile(true, (S32)sizeof(EntriesInfo));
    }
    else //update the header file first.
    {
        aprfile = openHeaderEntriesFile(false, 0);
        updatedHeaderEntriesFile() ;
        if(!aprfile)
        {
            return 0;
        }
        aprfile->seek(APR_SET, (S32)sizeof(EntriesInfo));
    }
    for (U32 idx=0; idx<num_entries; idx++)
    {
        try
        {
            Entry entry;
            S32 bytes_read = aprfile->read((void*)(&entry), (S32)sizeof(Entry));
            if (bytes_read < sizeof(Entry))
            {
                LL_WARNS() << "Corrupted header entries, failed at " << idx << " / " << num_entries << LL_ENDL;
                closeHeaderEntriesFile();
                return 0;
            }
            entries.push_back(entry);
            //      LL_INFOS() << "ENTRY: " << entry.mTime << " TEX: " << entry.mID << " IDX: " << idx << " Size: " << entry.mImageSize << LL_ENDL;
            if (entry.mImageSize > entry.mBodySize)
            {
                mHeaderIDMap[entry.mID] = idx;
                mTexturesSizeMap[entry.mID] = entry.mBodySize;
                mTexturesSizeTotal += entry.mBodySize;
            }
            else
            {
                mFreeList.insert(idx);
            }
        }
        catch (std::bad_alloc&)
        {
            // Too little ram yet very large cache?
            // Should this actually crash viewer?
            entries.clear();
            LL_WARNS() << "Bad alloc trying to read texture entries from cache, mFreeList: " << (S32)mFreeList.size()
                << ", added entries: " << idx << ", total entries: " << num_entries << LL_ENDL;
            closeHeaderEntriesFile();
            purgeAllTextures(false);
            return 0;
        }
    }
    closeHeaderEntriesFile();
    return num_entries;
}

void LLTextureCache::writeEntriesAndClose(const std::vector<Entry>& entries)
{
    auto num_entries = entries.size();
    llassert_always(num_entries == mHeaderEntriesInfo.mEntries);

    if (!mReadOnly)
    {
        LLAPRFile* aprfile = openHeaderEntriesFile(false, (S32)sizeof(EntriesInfo));
        for (size_t idx=0; idx<num_entries; idx++)
        {
            S32 bytes_written = aprfile->write((void*)(&entries[idx]), (S32)sizeof(Entry));
            if(bytes_written != sizeof(Entry))
            {
                clearCorruptedCache() ; //clear the cache.
                return ;
            }
        }
        closeHeaderEntriesFile();
    }
}

void LLTextureCache::writeUpdatedEntries()
{
    lockHeaders() ;
    if (!mReadOnly && !mUpdatedEntryMap.empty())
    {
        openHeaderEntriesFile(false, 0);
        updatedHeaderEntriesFile() ;
        closeHeaderEntriesFile();
    }
    unlockHeaders() ;
}

//mHeaderMutex is locked and mHeaderAPRFile is created before calling this.
void LLTextureCache::updatedHeaderEntriesFile()
{
    if (!mReadOnly && !mUpdatedEntryMap.empty() && mHeaderAPRFile)
    {
        //entriesInfo
        mHeaderAPRFile->seek(APR_SET, 0);
        S32 bytes_written = mHeaderAPRFile->write((U8*)&mHeaderEntriesInfo, sizeof(EntriesInfo)) ;
        if(bytes_written != sizeof(EntriesInfo))
        {
            clearCorruptedCache() ; //clear the cache.
            return ;
        }

        //write each updated entry
        S32 entry_size = (S32)sizeof(Entry) ;
        S32 prev_idx = -1 ;
        S32 delta_idx ;
        for (idx_entry_map_t::iterator iter = mUpdatedEntryMap.begin(); iter != mUpdatedEntryMap.end(); ++iter)
        {
            delta_idx = iter->first - prev_idx - 1;
            prev_idx = iter->first ;
            if(delta_idx)
            {
                mHeaderAPRFile->seek(APR_CUR, delta_idx * entry_size);
            }

            bytes_written = mHeaderAPRFile->write((void*)(&iter->second), entry_size);
            if(bytes_written != entry_size)
            {
                clearCorruptedCache() ; //clear the cache.
                return ;
            }
        }
        mUpdatedEntryMap.clear() ;
    }
}
//----------------------------------------------------------------------------

// Called from either the main thread or the worker thread
void LLTextureCache::readHeaderCache()
{
    mHeaderMutex.lock();

    mLRU.clear(); // always clear the LRU

    readEntriesHeader();

    if (mHeaderEntriesInfo.mVersion != sHeaderCacheVersion
        || mHeaderEntriesInfo.mAdressSize != sHeaderCacheAddressSize
        || strcmp(mHeaderEntriesInfo.mEncoderVersion, sHeaderCacheEncoderVersion.c_str()) != 0)
    {
        if (!mReadOnly)
        {
            LL_INFOS() << "Texture Cache version mismatch, Purging." << LL_ENDL;
            purgeAllTextures(false);
        }
    }
    else
    {
        std::vector<Entry> entries;
        U32 num_entries = openAndReadEntries(entries);
        if (num_entries)
        {
            U32 empty_entries = 0;
            typedef std::pair<U32, S32> lru_data_t;
            std::set<lru_data_t> lru;
            std::set<U32> purge_list;
            for (U32 i=0; i<num_entries; i++)
            {
                Entry& entry = entries[i];
                if (entry.mImageSize <= 0)
                {
                    // This will be in the Free List, don't put it in the LRU
                    ++empty_entries;
                }
                else
                {
                    lru.insert(std::make_pair(entry.mTime, i));
                    if (entry.mBodySize > 0)
                    {
                        if (entry.mBodySize > entry.mImageSize)
                        {
                            // Shouldn't happen, failsafe only
                            LL_WARNS() << "Bad entry: " << i << ": " << entry.mID << ": BodySize: " << entry.mBodySize << LL_ENDL;
                            purge_list.insert(i);
                        }
                    }
                }
            }
            if (num_entries - empty_entries > sCacheMaxEntries)
            {
                // Special case: cache size was reduced, need to remove entries
                U32 entries_to_purge = (num_entries - empty_entries) - sCacheMaxEntries;
                LL_INFOS() << "Texture Cache Entries: " << num_entries << " Max: " << sCacheMaxEntries << " Empty: " << empty_entries << " Purging: " << entries_to_purge << LL_ENDL;
                // We can exit the following loop with the given condition, since if we'd reach the end of the lru set we'd have:
                // purge_list.size() = lru.size() = num_entries - empty_entries = entries_to_purge + sCacheMaxEntries >= entries_to_purge
                // So, it's certain that iter will never reach lru.end() first.
                std::set<lru_data_t>::iterator iter = lru.begin();
                while (purge_list.size() < entries_to_purge)
                {
                    purge_list.insert(iter->second);
                    ++iter;
                }
            }

            {
                S32 lru_entries = (S32)((F32)sCacheMaxEntries * TEXTURE_CACHE_LRU_SIZE);
                for (std::set<lru_data_t>::iterator iter = lru.begin(); iter != lru.end(); ++iter)
                {
                    mLRU.insert(entries[iter->second].mID);
//                  LL_INFOS() << "LRU: " << iter->first << " : " << iter->second << LL_ENDL;
                    if (--lru_entries <= 0)
                        break;
                }
            }

            if (purge_list.size() > 0)
            {
                LLTimer timer;
                for (std::set<U32>::iterator iter = purge_list.begin(); iter != purge_list.end(); ++iter)
                {
                    std::string tex_filename = getTextureFileName(entries[*iter].mID);
                    removeEntry((S32)*iter, entries[*iter], tex_filename);

                    //make sure that pruning entries doesn't take too much time
                    if (timer.getElapsedTimeF32() > TEXTURE_PRUNING_MAX_TIME)
                    {
                        break;
                    }
                }
                writeEntriesAndClose(entries);
            }
            else
            {
                //entries are not changed, nothing here.
            }
        }
    }
    mHeaderMutex.unlock();
}

//////////////////////////////////////////////////////////////////////////////

//the header mutex is locked before calling this.
void LLTextureCache::clearCorruptedCache()
{
    LL_WARNS() << "the texture cache is corrupted, need to be cleared." << LL_ENDL ;

    closeHeaderEntriesFile();//close possible file handler
    purgeAllTextures(false) ; //clear the cache.

    if (!mReadOnly) //regenerate the directory tree if not exists.
    {
        LLFile::mkdir(mTexturesDirName);

        const char* subdirs = "0123456789abcdef";
        for (S32 i=0; i<16; i++)
        {
            std::string dirname = mTexturesDirName + gDirUtilp->getDirDelimiter() + subdirs[i];
            LLFile::mkdir(dirname);
        }
    }

    return ;
}

void LLTextureCache::purgeAllTextures(bool purge_directories)
{
    // <SS:Nexii> Squeeze - every cache-clear path in the viewer funnels through here, including the mid-session one, so this is the single place the BC7 sidecar has to die too. It is a complete second copy of every texture the user has looked at; leaving it behind would make "clear cache" a lie. It goes first because the Windows branch below renames this whole directory away, and the store has to have dropped its own state before that happens.
    if (SSBC7Store::instanceExists())
    {
        SSBC7Store::instance().purgeAll();
    }
    // </SS:Nexii>

    // <SS:Nexii> Strata - and the body volumes with it, for the same reason and with the same urgency. The non-directory branch below only calls deleteFilesInDir on this directory, which does not descend into the `strata` child at all, so without this line a "clear cache" driven by a header version mismatch would leave every volume standing and the next session would open an index describing bodies whose entries no longer exist.
    //
    // The uninitialised branch is not defensive padding: LLAppViewer::initCache calls purgeCache() on this object BEFORE initCache() has told the store where it lives, so at that point the store cannot name its own directory and purgeAll would decline. Naming the path here is the only way that wipe reaches the volumes, and on a platform without the Windows rename shortcut below the alternative is stranding gigabytes the next session's index cannot even see.
    {
        SSStrataStore& tex_strata = SSStrataStore::tier(SSSTRATA_TENANT_TEXTURES);
        if (tex_strata.isInitialized())
        {
            tex_strata.purgeAll();
        }
        else if (!mReadOnly)
        {
            gDirUtilp->deleteDirAndContents(mTexturesDirName + gDirUtilp->getDirDelimiter() + SSSTRATA_DIR_NAME);
        }
    }
    // </SS:Nexii>

    if (!mReadOnly)
    {
// <FS:ND> Windows can be really slow deleting a huge texture cache.
// In case of a full purge rename the directory and then purge this using a low priority background thread.
#if LL_WINDOWS
        bool bOldPurge = true;
        if( purge_directories )
        {
            std::string strNewName = mCacheParentDirName + gDirUtilp->getDirDelimiter() + LLUUID::generateNewID().asString() + ".old_texturecache";
            if( LLFile::isdir( mTexturesDirName ) )
                bOldPurge = ( 0 != LLFile::rename( mTexturesDirName, strNewName ) );

            if( bOldPurge )
            {
                LL_WARNS() << "Rename of " << mTexturesDirName << " to " << strNewName << " failed" << LL_ENDL;
            }
        }
        if( !purge_directories || bOldPurge )
#endif
// </FS:ND>
        {
        const char* subdirs = "0123456789abcdef";
        std::string delem = gDirUtilp->getDirDelimiter();
        std::string mask = "*";
        for (S32 i=0; i<16; i++)
        {
            std::string dirname = mTexturesDirName + delem + subdirs[i];
            LL_INFOS() << "Deleting files in directory: " << dirname << LL_ENDL;
            if (purge_directories)
            {
                gDirUtilp->deleteDirAndContents(dirname);
            }
            else
            {
                gDirUtilp->deleteFilesInDir(dirname, mask);
            }
#if LL_WINDOWS
            // Texture cache can be large and can take a while to remove
            // assure OS that processes is alive and not hanging
            MSG msg;
            PeekMessage(&msg, 0, 0, 0, PM_NOREMOVE | PM_NOYIELD);
#endif
        }
        // <FS:Ansariel> Only delete folder if it actually exist
        if (LLFile::isdir(mTexturesDirName))
        {
        // </FS:Ansariel>
        gDirUtilp->deleteFilesInDir(mTexturesDirName, mask); // headers, fast cache
        if (purge_directories)
        {
            LLFile::rmdir(mTexturesDirName);
        }
        // <FS:Ansariel> Only delete folder if it actually exist
        }
        // </FS:Ansariel>
        }
    }
    {
        LLMutexLock lock(&mHeaderIDMapMutex);
        mHeaderIDMap.clear();
    }
    mTexturesSizeMap.clear();
    mTexturesSizeTotal = 0;
    mFreeList.clear();
    mTexturesSizeTotal = 0;
    mUpdatedEntryMap.clear();

    // Info with 0 entries
    setEntriesHeader();
    writeEntriesHeader();

    LL_INFOS() << "The entire texture cache is cleared." << LL_ENDL ;
}

void LLTextureCache::purgeTexturesLazy(F32 time_limit_sec)
{
    if (mReadOnly)
    {
        return;
    }

    if (!mThreaded)
    {
        LLAppViewer::instance()->pauseMainloopTimeout();
    }

    // time_limit doesn't account for lock time
    LLMutexLock lock(&mHeaderMutex);

    if (mPurgeEntryList.empty())
    {
        // Read the entries list and form list of textures to purge
        std::vector<Entry> entries;
        U32 num_entries = openAndReadEntries(entries);
        if (!num_entries)
        {
            return; // nothing to purge
        }

        // Use mTexturesSizeMap to collect UUIDs of textures with bodies
        typedef std::set<std::pair<U32, S32> > time_idx_set_t;
        std::set<std::pair<U32, S32> > time_idx_set;
        for (size_map_t::iterator iter1 = mTexturesSizeMap.begin();
            iter1 != mTexturesSizeMap.end(); ++iter1)
        {
            if (iter1->second > 0)
            {
                LLMutexLock lock(&mHeaderIDMapMutex);
                id_map_t::iterator iter2 = mHeaderIDMap.find(iter1->first);
                if (iter2 != mHeaderIDMap.end())
                {
                    S32 idx = iter2->second;
                    time_idx_set.insert(std::make_pair(entries[idx].mTime, idx));
                }
                else
                {
                    LL_ERRS("TextureCache") << "mTexturesSizeMap / mHeaderIDMap corrupted." << LL_ENDL;
                }
            }
        }

        S64 cache_size = mTexturesSizeTotal;
        S64 purged_cache_size = (llmax(cache_size, sCacheMaxTexturesSize.load()) * (S64)((1.f - TEXTURE_CACHE_PURGE_AMOUNT) * 100)) / 100;   // <SS:Nexii/> atomic load only - this path re-reads the budget every pass, which is exactly what lets the arbiter move it mid-session
        for (time_idx_set_t::iterator iter = time_idx_set.begin();
            iter != time_idx_set.end(); ++iter)
        {
            S32 idx = iter->second;
            if (cache_size >= purged_cache_size)
            {
                cache_size -= entries[idx].mBodySize;
                mPurgeEntryList.push_back(std::pair<S32, Entry>(idx, entries[idx]));
            }
            else
            {
                break;
            }
        }
        LL_DEBUGS("TextureCache") << "Formed Purge list of " << mPurgeEntryList.size() << " entries" << LL_ENDL;
    }
    else
    {
        // Remove collected entried
        LLTimer timer;
        while (!mPurgeEntryList.empty() && timer.getElapsedTimeF32() < time_limit_sec)
        {
            S32 idx = mPurgeEntryList.back().first;
            Entry entry = mPurgeEntryList.back().second;
            mPurgeEntryList.pop_back();
            // make sure record is still valid
            bool remove_entry = false;
            {
                LLMutexLock lock(&mHeaderIDMapMutex);
                id_map_t::iterator iter_header = mHeaderIDMap.find(entry.mID);
                remove_entry = (iter_header != mHeaderIDMap.end() && iter_header->second == idx);
            }
            if (remove_entry)
            {
                std::string tex_filename = getTextureFileName(entry.mID);
                removeEntry(idx, entry, tex_filename);
                writeEntryToHeaderImmediately(idx, entry);
            }
        }
    }
}

void LLTextureCache::purgeTextures(bool validate)
{
    if (mReadOnly)
    {
        return;
    }

    if (!mThreaded)
    {
        // *FIX:Mani - watchdog off.
        LLAppViewer::instance()->pauseMainloopTimeout();
    }

    LLMutexLock lock(&mHeaderMutex);

    LL_INFOS() << "TEXTURE CACHE: Purging." << LL_ENDL;

    // Read the entries list
    std::vector<Entry> entries;
    U32 num_entries = openAndReadEntries(entries);
    if (!num_entries)
    {
        return; // nothing to purge
    }

    // Use mTexturesSizeMap to collect UUIDs of textures with bodies
    typedef std::set<std::pair<U32,S32> > time_idx_set_t;
    std::set<std::pair<U32,S32> > time_idx_set;
    for (size_map_t::iterator iter1 = mTexturesSizeMap.begin();
         iter1 != mTexturesSizeMap.end(); ++iter1)
    {
        if (iter1->second > 0)
        {
            LLMutexLock lock(&mHeaderIDMapMutex);
            id_map_t::iterator iter2 = mHeaderIDMap.find(iter1->first);
            if (iter2 != mHeaderIDMap.end())
            {
                S32 idx = iter2->second;
                time_idx_set.insert(std::make_pair(entries[idx].mTime, idx));
//              LL_INFOS() << "TIME: " << entries[idx].mTime << " TEX: " << entries[idx].mID << " IDX: " << idx << " Size: " << entries[idx].mImageSize << LL_ENDL;
            }
            else
            {
                LL_ERRS() << "mTexturesSizeMap / mHeaderIDMap corrupted." << LL_ENDL ;
            }
        }
    }

    // Validate 1/256th of the files on startup
    U32 validate_idx = 0;
    if (validate)
    {
        validate_idx = gSavedSettings.getU32("CacheValidateCounter");
        U32 next_idx = (validate_idx + 1) % 256;
        gSavedSettings.setU32("CacheValidateCounter", next_idx);
        LL_DEBUGS("TextureCache") << "TEXTURE CACHE: Validating: " << validate_idx << LL_ENDL;
    }

    S64 cache_size = mTexturesSizeTotal;
    S64 purged_cache_size = (llmax(cache_size, sCacheMaxTexturesSize.load()) * (S64)((1.f - TEXTURE_CACHE_PURGE_AMOUNT) * 100)) / 100;   // <SS:Nexii/> atomic load only - the second of the two purge paths that re-read the budget live
    S32 purge_count = 0;
    for (time_idx_set_t::iterator iter = time_idx_set.begin();
         iter != time_idx_set.end(); ++iter)
    {
        S32 idx = iter->second;
        bool purge_entry = false;

        if (cache_size >= purged_cache_size)
        {
            purge_entry = true;
        }
        else if (validate)
        {
            // make sure file exists and is the correct size
            U32 uuididx = entries[idx].mID.mData[0];
            if (uuididx == validate_idx)
            {
                std::string filename = getTextureFileName(entries[idx].mID);
                LL_DEBUGS("TextureCache") << "Validating: " << filename << "Size: " << entries[idx].mBodySize << LL_ENDL;
                // mHeaderAPRFilePoolp because this is under header mutex in main thread
                // <SS:Nexii> Strata - a packed body has no file, so asking the filesystem would report zero and this check would purge every packed entry it sampled. bodySize answers from whichever tier actually holds the bytes.
                S32 bodysize = bodySize(entries[idx].mID, filename, mHeaderAPRFilePoolp);
                // </SS:Nexii>
                if (bodysize != entries[idx].mBodySize)
                {
                    LL_WARNS("TextureCache") << "TEXTURE CACHE BODY HAS BAD SIZE: " << bodysize << " != " << entries[idx].mBodySize << filename << LL_ENDL;
                    purge_entry = true;
                }
            }
        }
        else
        {
            break;
        }

        if (purge_entry)
        {
            purge_count++;
            std::string filename = getTextureFileName(entries[idx].mID);
            LL_DEBUGS("TextureCache") << "PURGING: " << filename << LL_ENDL;
            cache_size -= entries[idx].mBodySize;
            removeEntry(idx, entries[idx], filename) ;
        }
    }

    LL_DEBUGS("TextureCache") << "TEXTURE CACHE: Writing Entries: " << num_entries << LL_ENDL;

    writeEntriesAndClose(entries);

    // *FIX:Mani - watchdog back on.
    LLAppViewer::instance()->resumeMainloopTimeout();

    LL_INFOS("TextureCache") << "TEXTURE CACHE:"
            << " PURGED: " << purge_count
            << " ENTRIES: " << num_entries
            << " CACHE SIZE: " << mTexturesSizeTotal / (1024 * 1024) << " MB"
            << LL_ENDL;
}

//////////////////////////////////////////////////////////////////////////////

// call lockWorkers() first!
LLTextureCacheWorker* LLTextureCache::getReader(handle_t handle)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLTextureCacheWorker* res = NULL;
    handle_map_t::iterator iter = mReaders.find(handle);
    if (iter != mReaders.end())
    {
        res = iter->second;
    }
    return res;
}

LLTextureCacheWorker* LLTextureCache::getWriter(handle_t handle)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLTextureCacheWorker* res = NULL;
    handle_map_t::iterator iter = mWriters.find(handle);
    if (iter != mWriters.end())
    {
        res = iter->second;
    }
    return res;
}

//////////////////////////////////////////////////////////////////////////////
// Called from work thread

// Reads imagesize from the header, updates timestamp
S32 LLTextureCache::getHeaderCacheEntry(const LLUUID& id, Entry& entry)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLMutexLock lock(&mHeaderMutex);
    S32 idx = openAndReadEntry(id, entry, false);
    if (idx >= 0)
    {
        updateEntryTimeStamp(idx, entry); // updates time
    }
    return idx;
}

// Writes imagesize to the header, updates timestamp
S32 LLTextureCache::setHeaderCacheEntry(const LLUUID& id, Entry& entry, S32 imagesize, S32 datasize)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    mHeaderMutex.lock();
    S32 idx = openAndReadEntry(id, entry, true); // read or create
    mHeaderMutex.unlock();

    if(idx < 0) // retry once
    {
        readHeaderCache(); // We couldn't write an entry, so refresh the LRU

        mHeaderMutex.lock();
        idx = openAndReadEntry(id, entry, true);
        mHeaderMutex.unlock();
    }

    if (idx >= 0)
    {
        updateEntry(idx, entry, imagesize, datasize);
    }
    else
    {
        LL_WARNS() << "Failed to set cache entry for image: " << id << LL_ENDL;
        // We couldn't write to file, switch to read only mode and clear data
        setReadOnly(true);
        clearCorruptedCache(); // won't remove files due to "read only"
    }

    return idx;
}

//////////////////////////////////////////////////////////////////////////////

// Calls from texture pipeline thread (i.e. LLTextureFetch)

LLTextureCache::handle_t LLTextureCache::readFromCache(const std::string& filename, const LLUUID& id,
                                                       S32 offset, S32 size, ReadResponder* responder)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    // Note: checking to see if an entry exists can cause a stall,
    //  so let the thread handle it
    LLMutexLock lock(&mWorkersMutex);
    LLTextureCacheWorker* worker = new LLTextureCacheLocalFileWorker(this, filename, id,
                                                                     NULL, size, offset, 0,
                                                                     responder);
    handle_t handle = worker->read();
    mReaders[handle] = worker;
    return handle;
}

LLTextureCache::handle_t LLTextureCache::readFromCache(const LLUUID& id,
                                                       S32 offset, S32 size, ReadResponder* responder)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    // Note: checking to see if an entry exists can cause a stall,
    //  so let the thread handle it
    LLMutexLock lock(&mWorkersMutex);
    LLTextureCacheWorker* worker = new LLTextureCacheRemoteWorker(this, id,
                                                                  NULL, size, offset,
                                                                  0, NULL, 0, responder);
    handle_t handle = worker->read();
    mReaders[handle] = worker;
    return handle;
}


bool LLTextureCache::readComplete(handle_t handle, bool abort)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    lockWorkers();
    handle_map_t::iterator iter = mReaders.find(handle);
    LLTextureCacheWorker* worker = NULL;
    bool complete = false;
    if (iter != mReaders.end())
    {
        worker = iter->second;
        complete = worker->complete();

        if(!complete && abort)
        {
            abortRequest(handle, true) ;
        }
    }
    if (worker && (complete || abort))
    {
        mReaders.erase(iter);
        unlockWorkers();
        worker->scheduleDelete();
    }
    else
    {
        unlockWorkers();
    }
    return (complete || abort);
}

LLTextureCache::handle_t LLTextureCache::writeToCache(const LLUUID& id,
                                                      const U8* data, S32 datasize, S32 imagesize,
                                                      LLPointer<LLImageRaw> rawimage, S32 discardlevel,
                                                      WriteResponder* responder)
{
    if (mReadOnly)
    {
        delete responder;
        return LLWorkerThread::nullHandle();
    }
    if (mDoPurge)
    {
        // NOTE: Needs to be done on the control thread
        //  (i.e. here)
        purgeTexturesLazy(TEXTURE_LAZY_PURGE_TIME_LIMIT);
        mDoPurge = !mPurgeEntryList.empty();
    }

    // <FS:ND> There seems to be an edge case of KDU failing to decode images and then we end with null data here.
    // This should be bettered handled up where it fails, but at least this stops the crashes.
    if( rawimage.isNull() || !rawimage->getData() )
    {
        delete responder;
        return LLWorkerThread::nullHandle();
    }
    // </FS:ND>

    LLMutexLock lock(&mWorkersMutex);
    LLTextureCacheWorker* worker = new LLTextureCacheRemoteWorker(this, id,
                                                                  data, datasize, 0,
                                                                  imagesize, rawimage, discardlevel, responder);
    handle_t handle = worker->write();
    mWriters[handle] = worker;
    return handle;
}

//called in the main thread
LLPointer<LLImageRaw> LLTextureCache::readFromFastCache(const LLUUID& id, S32& discardlevel)
{
    U32 offset;
    {
        LLMutexLock lock(&mHeaderIDMapMutex);
        id_map_t::const_iterator iter = mHeaderIDMap.find(id);
        if(iter == mHeaderIDMap.end())
        {
            return NULL; //not in the cache
        }

        offset = iter->second;
    }
    offset *= TEXTURE_FAST_CACHE_ENTRY_SIZE;

    U8* data;
    S32 head[4];
    {
        LL_PROFILE_ZONE_NAMED("Read fast cache");
        LLMutexLock lock(&mFastCacheMutex);

        openFastCache(); // only reopens if needed, lasts 10 seconds

        mFastCachep->seek(APR_SET, offset);

        if(mFastCachep->read(head, TEXTURE_FAST_CACHE_ENTRY_OVERHEAD) != TEXTURE_FAST_CACHE_ENTRY_OVERHEAD)
        {
            //cache corrupted or under thread race condition
            closeFastCache();
            return NULL;
        }

        S32 image_size = head[0] * head[1] * head[2];
        if(image_size <= 0
           || image_size > TEXTURE_FAST_CACHE_DATA_SIZE
           || head[3] < 0) //invalid
        {
            closeFastCache();
            return NULL;
        }
        discardlevel = head[3];

        data = (U8*)ll_aligned_malloc_16(image_size);
        if(mFastCachep->read(data, image_size) != image_size)
        {
            ll_aligned_free_16(data);
            closeFastCache();
            return NULL;
        }

        closeFastCache();
    }

    // directly construct image from new buffer.
    LLPointer<LLImageRaw> raw = new LLImageRaw(data, head[0], head[1], head[2], true /*take ownership*/);

    // <FS:minerjr>
    // This fixes the invalid discard values from being created which cause the decoder code to fail when trying to handle 6 and 7's which are above the MAX_DISCARD_LEVEL of 5
    // especially on load
    // We will expand the 16x16 texture to the actual MAX_DISCARD_LEVEL texture size, it may be blurry until the user gets closer but 5 discard value should be objects far from the camera.
    // So a 1024x1024 texture with a dicard of 6 will become 32x32 and a 2048x2048 texture with a discard of 7 will become a 64x64 texture.
    if (discardlevel > MAX_DISCARD_LEVEL)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("FixBadDiscardLevel");

        S32 w = head[0]; // Get the current width from the header (16)
        S32 h = head[1]; // Get the current height from the header (16)

        // Expand the width and height by teh difference between the discard and MAX_DISCARD_LEVEL bit shifted to the left. (Expand power of 2 textures)
        w <<= discardlevel - MAX_DISCARD_LEVEL;
        h <<= discardlevel - MAX_DISCARD_LEVEL;

        // Set the discard level to the MAX_DISCARD_LEVEL
        discardlevel = MAX_DISCARD_LEVEL;

        // Scale up the texture and scale the actual data, as we just created it above, it should be fine.
        raw->scale(w, h, true);
    }
    // </FS:minerjr>

    return raw;
}

//return the fast cache location
bool LLTextureCache::writeToFastCache(LLUUID image_id, S32 id, LLPointer<LLImageRaw> raw, S32 discardlevel)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    LLImageDataSharedLock lock(raw);

    //rescale image if needed
    if (raw.isNull() || raw->isBufferInvalid() || !raw->getData())
    {
        LL_ERRS() << "Attempted to write NULL raw image to fastcache" << LL_ENDL;
        return false;
    }

    S32 w, h, c;
    w = raw->getWidth();
    h = raw->getHeight();
    c = raw->getComponents();

    S32 i = 0 ;

    // Search for a discard level that will fit into fast cache
    while(((w >> i) * (h >> i) * c) > TEXTURE_FAST_CACHE_DATA_SIZE)
    {
        ++i ;
    }

    if(i)
    {
        w >>= i;
        h >>= i;
        if(w * h *c > 0) //valid
        {
            // Make a duplicate to keep the original raw image untouched.
            raw = raw->duplicate();

            if (raw->isBufferInvalid())
            {
                LL_WARNS() << "Invalid image duplicate buffer" << LL_ENDL;
                return false;
            }

            raw->scale(w, h);

            discardlevel += i ;
        }
    }

    //copy data
    memcpy(mFastCachePadBuffer, &w, sizeof(S32));
    memcpy(mFastCachePadBuffer + sizeof(S32), &h, sizeof(S32));
    memcpy(mFastCachePadBuffer + sizeof(S32) * 2, &c, sizeof(S32));
    memcpy(mFastCachePadBuffer + sizeof(S32) * 3, &discardlevel, sizeof(S32));

    S32 copy_size = w * h * c;
    if(copy_size > 0) //valid
    {
        copy_size = llmin(copy_size, TEXTURE_FAST_CACHE_ENTRY_SIZE - TEXTURE_FAST_CACHE_ENTRY_OVERHEAD);
        memcpy(mFastCachePadBuffer + TEXTURE_FAST_CACHE_ENTRY_OVERHEAD, raw->getData(), copy_size);
    }
    S32 offset = id * TEXTURE_FAST_CACHE_ENTRY_SIZE;

    {
        LLMutexLock lock(&mFastCacheMutex);

        openFastCache();

        mFastCachep->seek(APR_SET, offset);

        //no need to do this assertion check. When it fails, let it fail quietly.
        //this failure could happen because other viewer removes the fast cache file when clearing cache.
        //--> llassert_always(mFastCachep->write(mFastCachePadBuffer, TEXTURE_FAST_CACHE_ENTRY_SIZE) == TEXTURE_FAST_CACHE_ENTRY_SIZE);
        mFastCachep->write(mFastCachePadBuffer, TEXTURE_FAST_CACHE_ENTRY_SIZE);

        closeFastCache(true);
    }

    return true;
}

void LLTextureCache::openFastCache(bool first_time)
{
    if(!mFastCachep)
    {
        if(first_time)
        {
            if(!mFastCachePadBuffer)
            {
                mFastCachePadBuffer = (U8*)ll_aligned_malloc_16(TEXTURE_FAST_CACHE_ENTRY_SIZE);
            }
            mFastCachePoolp = new LLVolatileAPRPool(); // is_local= true by default, so not thread safe by default
            if (LLAPRFile::isExist(mFastCacheFileName, mFastCachePoolp))
            {
                mFastCachep = new LLAPRFile(mFastCacheFileName, APR_READ|APR_WRITE|APR_BINARY, mFastCachePoolp) ;
            }
            else
            {
                mFastCachep = new LLAPRFile(mFastCacheFileName, APR_CREATE|APR_READ|APR_WRITE|APR_BINARY, mFastCachePoolp) ;
            }
        }
        else
        {
            mFastCachep = new LLAPRFile(mFastCacheFileName, APR_READ|APR_WRITE|APR_BINARY, mFastCachePoolp) ;
        }

        mFastCacheTimer.reset();
    }
    return;
}

void LLTextureCache::closeFastCache(bool forced)
{
    static const F32 timeout = 10.f ; //seconds

    if(!mFastCachep)
    {
        return ;
    }

    if(!forced && mFastCacheTimer.getElapsedTimeF32() < timeout)
    {
        return ;
    }

    delete mFastCachep;
    mFastCachep = NULL;
    return;
}

bool LLTextureCache::writeComplete(handle_t handle, bool abort)
{
    lockWorkers();
    handle_map_t::iterator iter = mWriters.find(handle);
    llassert(iter != mWriters.end());
    if (iter != mWriters.end())
    {
        LLTextureCacheWorker* worker = iter->second;
        if (worker->complete() || abort)
        {
            mWriters.erase(handle);
            unlockWorkers();
            worker->scheduleDelete();
            return true;
        }
    }
    unlockWorkers();
    return false;
}

void LLTextureCache::prioritizeWrite(handle_t handle)
{
    // Don't prioritize yet, we might be working on this now
    //   which could create a deadlock
    LLMutexLock lock(&mListMutex);
    mPrioritizeWriteList.push_back(handle);
}

void LLTextureCache::addCompleted(Responder* responder, bool success)
{
    LLMutexLock lock(&mListMutex);
    mCompletedList.push_back(std::make_pair(responder,success));
}

//////////////////////////////////////////////////////////////////////////////

//called after mHeaderMutex is locked.
void LLTextureCache::removeCachedTexture(const LLUUID& id)
{
    if(mTexturesSizeMap.find(id) != mTexturesSizeMap.end())
    {
        mTexturesSizeTotal -= mTexturesSizeMap[id] ;
        mTexturesSizeMap.erase(id);
    }
    {
        LLMutexLock lock(&mHeaderIDMapMutex);
        mHeaderIDMap.erase(id);
    }
    // We are inside header's mutex so mHeaderAPRFilePoolp is safe to use,
    // but getLocalAPRFilePool() is not safe, it might be in use by worker
    forgetBody(id);   // <SS:Nexii/> Strata - a packed body is not a file, so the unlink below cannot free it; the record has to be revoked or the bytes stay allocated until the whole volume is reclaimed
    LLAPRFile::remove(getTextureFileName(id), mHeaderAPRFilePoolp);
}

//called after mHeaderMutex is locked.
void LLTextureCache::removeEntry(S32 idx, Entry& entry, std::string& filename)
{
    bool file_maybe_exists = true;  // Always attempt to remove when idx is invalid.

    if(idx >= 0) //valid entry
    {
        if (entry.mBodySize == 0)   // Always attempt to remove when mBodySize > 0.
        {
          // Sanity check. Shouldn't exist when body size is 0.
          // We are inside header's mutex so mHeaderAPRFilePoolp is safe to use,
          // but getLocalAPRFilePool() is not safe, it might be in use by worker
          if (LLAPRFile::isExist(filename, mHeaderAPRFilePoolp))
          {
              LL_WARNS("TextureCache") << "Entry has body size of zero but file " << filename << " exists. Deleting this file, too." << LL_ENDL;
          }
          else
          {
              file_maybe_exists = false;
          }
        }
        mTexturesSizeTotal -= entry.mBodySize;

        entry.mImageSize = -1;
        entry.mBodySize = 0;
        {
            LLMutexLock lock(&mHeaderIDMapMutex);
            mHeaderIDMap.erase(entry.mID);
        }
        mTexturesSizeMap.erase(entry.mID);
        mFreeList.insert(idx);
    }

    if (file_maybe_exists)
    {
        forgetBody(entry.mID);   // <SS:Nexii/> Strata - same reason as removeCachedTexture: every eviction path has to revoke the packed record as well as unlink the loose file, or the volumes grow against a live-byte total that is falling
        LLAPRFile::remove(filename, mHeaderAPRFilePoolp);
    }
}

bool LLTextureCache::removeFromCache(const LLUUID& id)
{
    //LL_WARNS() << "Removing texture from cache: " << id << LL_ENDL;
    bool ret = false ;
    if (!mReadOnly)
    {
        lockHeaders() ;

        Entry entry;
        S32 idx = openAndReadEntry(id, entry, false);
        std::string tex_filename = getTextureFileName(id);
        removeEntry(idx, entry, tex_filename) ;
        if (idx >= 0)
        {
            writeEntryToHeaderImmediately(idx, entry);
            ret = true;
        }

        unlockHeaders() ;
    }
    return ret ;
}

//////////////////////////////////////////////////////////////////////////////
// <SS:Nexii> Strata - the J2C body tier
//
// WHY THIS IS A STAGING TIER AND NOT A DIRECT CONTAINER. Body writes are left exactly where they were: one loose file per uuid, written by the cache worker with the same syscall and the same failure modes as before. A background packer folds them into volumes once they have stopped changing. That ordering is what satisfies "no main-thread stall, and fetch threads must not block on cache IO" without having to argue about it - no thread the user can feel ever waits on volume IO, because the only thread that writes a volume is the once-a-minute maintenance thread that already existed. It is also what makes the existing 15,090 body files absorbable rather than disposable: they ARE the staging tier by construction, so they are folded away over the first few minutes of runtime instead of being deleted and re-fetched.
//
// Unlike the asset tenant, nothing here ever mutates a packed object. A better discard level rewrites the body whole, which is a revoke followed by a fresh loose write, so the unstable-set machinery the asset tier needs for llmeshrepository's LOD patching never fires on this side.

S32 LLTextureCache::bodySize(const LLUUID& id, const std::string& filename, LLVolatileAPRPool* pool)
{
    if (SSStrataStore* strata = SSStrataStore::live(SSSTRATA_TENANT_TEXTURES))
    {
        S32 packed = 0;
        if (strata->objectSize(id, packed)) return packed;
    }
    return LLAPRFile::size(filename, pool);
}

S32 LLTextureCache::readBody(const LLUUID& id, const std::string& filename, S32 offset, U8* dst, S32 bytes, LLVolatileAPRPool* pool)
{
    if (SSStrataStore* strata = SSStrataStore::live(SSSTRATA_TENANT_TEXTURES))
    {
        S32 got = 0;
        if (strata->readObject(id, offset, dst, bytes, got)) return got;

        // FALL THROUGH RATHER THAN PROBE. readObject returns false both for the overwhelmingly common "this uuid was never packed" - which costs one hash lookup and says nothing - and for a genuine read failure, which has already revoked the record and logged why: open, short, identity or CRC, each counted separately. The two resolve the same way, so the loose read below is simply attempted. Probing for the loose file first to tell them apart would spend a stat on EVERY body read to make one log line nicer, and when there is no loose file the readEx returns short and doRead reports it exactly as it always has.
        //
        // An object mid-pack can be in the index and on disk at the same time, and the crash rule guarantees the loose copy is the authoritative one whenever both exist - which is the other reason this direction of fallback is the correct one.
    }
    return LLAPRFile::readEx(filename, dst, offset, bytes, pool);
}

bool LLTextureCache::writeBody(const LLUUID& id, const std::string& filename, const U8* src, S32 bytes)
{
    if (SSStrataStore* strata = SSStrataStore::live(SSSTRATA_TENANT_TEXTURES))
    {
        // restore_bytes is FALSE, and that is the whole difference between this tenant and the asset one: the caller is about to write the complete body, so handing the old payload back to the loose file would only be overwritten a syscall later. The record is revoked either way, which is what matters.
        strata->prepareForWrite(id, LLAssetType::AT_TEXTURE, filename, false);
    }
    return LLAPRFile::writeEx(filename, const_cast<U8*>(src), 0, bytes, getLocalAPRFilePool()) > 0;
}

void LLTextureCache::forgetBody(const LLUUID& id)
{
    if (SSStrataStore* strata = SSStrataStore::live(SSSTRATA_TENANT_TEXTURES))
    {
        strata->forget(id);
    }
}

// <SS:Nexii> Squeeze promotion - see the header for the thread contract. Everything here mirrors LLTextureCacheRemoteWorker::doRead's HEADER and BODY stages byte for byte, because the promotion engine has to see exactly what the fetch pipeline would see; the differences are that nothing is mutated, the whole asset is always asked for, and the entry is re-read afterwards so a reclaimed index cannot silently hand back another texture's bytes.
const char* LLTextureCache::ssJ2CProbeName(ESSJ2CProbe probe)
{
    switch (probe)
    {
        case SS_J2C_PROBE_COMPLETE:    return "complete";
        case SS_J2C_PROBE_NO_ENTRY:    return "no cache entry";
        case SS_J2C_PROBE_UNKNOWN:     return "total size unknown";
        case SS_J2C_PROBE_PARTIAL:     return "partial";
        case SS_J2C_PROBE_READ_FAILED: return "read failed";
        case SS_J2C_PROBE_RACED:       return "entry changed under the read";
        default:                       return "unknown";
    }
}

LLTextureCache::ESSJ2CProbe LLTextureCache::ssProbeJ2C(const LLUUID& id, bool want_bytes, std::vector<U8>& out_data, S32& out_have, S32& out_total, LLVolatileAPRPool* pool)
{
    out_data.clear();
    out_have  = 0;
    out_total = 0;

    if (id.isNull()) return SS_J2C_PROBE_NO_ENTRY;

    S32   idx = -1;
    Entry entry;
    {
        // create is FALSE, which is what makes this a pure read: openAndReadEntry's whole allocate-an-index branch is skipped and a miss simply returns -1.
        LLMutexLock lock(&mHeaderMutex);
        idx = openAndReadEntry(id, entry, false);
    }
    if (idx < 0) return SS_J2C_PROBE_NO_ENTRY;

    const ESSBC7J2CExtent extent = ssBC7J2CExtent(entry.mImageSize, entry.mBodySize, TEXTURE_CACHE_ENTRY_SIZE, out_have, out_total);
    if (extent == SSBC7_J2C_UNKNOWN)  return SS_J2C_PROBE_UNKNOWN;
    if (extent == SSBC7_J2C_PARTIAL)  return SS_J2C_PROBE_PARTIAL;
    if (!want_bytes)                  return SS_J2C_PROBE_COMPLETE;

    const S32 total = out_total;
    if (total <= 0 || total > MAX_IMAGE_DATA_SIZE) return SS_J2C_PROBE_UNKNOWN;

    out_data.resize((size_t)total);

    // A null pool would send the reads below to LLAPRFile's global pool, and APR pools are not thread safe. The BC7Encode workers do this concurrently with each other and with the main thread's own file use, which corrupts that pool's cleanup list until closing the logout marker at quit spins forever. So each calling thread gets its own pool. It is never deleted on purpose: apr_terminate() reclaims its APR memory with every other root pool, and deleting it from a thread that outlived ll_cleanup_apr() would free that memory twice.
    if (!pool)
    {
        static thread_local LLVolatileAPRPool* thread_pool = new LLVolatileAPRPool();
        pool = thread_pool;
    }

    // The first record's worth lives in texture.cache at a fixed stride, exactly as the HEADER stage reads it. A texture smaller than one record occupies a whole padded record, so the read is clamped to what the asset actually is rather than to the record size.
    const S32 header_bytes = llmin(total, (S32)TEXTURE_CACHE_ENTRY_SIZE);
    const S32 header_read  = LLAPRFile::readEx(mHeaderDataFileName, &out_data[0], idx * TEXTURE_CACHE_ENTRY_SIZE, header_bytes, pool);
    if (header_read != header_bytes)
    {
        LL_WARNS("Squeeze") << "promotion read of " << id << " got " << header_read << " of " << header_bytes
                            << " header bytes from the texture cache, so the texture is left to the ordinary path" << LL_ENDL;
        out_data.clear();
        return SS_J2C_PROBE_READ_FAILED;
    }

    const S32 body_bytes = total - header_bytes;
    if (body_bytes > 0)
    {
        const std::string filename = getTextureFileName(id);
        const S32 body_read = readBody(id, filename, 0, &out_data[header_bytes], body_bytes, pool);
        if (body_read != body_bytes)
        {
            LL_WARNS("Squeeze") << "promotion read of " << id << " got " << body_read << " of " << body_bytes
                                << " body bytes, so the entry claims more than the disk holds and the texture is left to the ordinary path" << LL_ENDL;
            out_data.clear();
            return SS_J2C_PROBE_READ_FAILED;
        }
    }

    // Index slots are recycled, so between the two reads above this slot could have been handed to a different texture by removeCachedTexture - at which point the bytes in hand are a perfectly well formed J2C for somebody else and would be stored under this uuid forever. Re-reading the entry is the cheap version of the uuid check the BC7 store makes on every blob.
    {
        LLMutexLock lock(&mHeaderMutex);
        Entry after;
        const S32 idx_after = openAndReadEntry(id, after, false);
        if (idx_after != idx || after.mImageSize != entry.mImageSize || after.mBodySize != entry.mBodySize)
        {
            LL_WARNS("Squeeze") << "promotion read of " << id << " was abandoned: its cache entry moved from index " << idx
                                << " to " << idx_after << " while the bytes were being read" << LL_ENDL;
            out_data.clear();
            return SS_J2C_PROBE_RACED;
        }
    }

    return SS_J2C_PROBE_COMPLETE;
}
// </SS:Nexii>

// Copies a BOUNDED slice of mTexturesSizeMap and nothing else. The bound is the point: on a cold cache this map holds fifteen thousand entries, and building that many paths under mHeaderMutex would park the main thread's own lazy purge behind this tick once a minute. A cursor makes the next tick continue where this one stopped, so a full sweep still happens, just spread over several minutes - which is the pace the packer's own byte budget wants anyway.
bool LLTextureCache::strataCollectCandidates(std::vector<SSStrataLooseFile>& out)
{
    out.clear();

    bool wrapped = false;
    std::vector<std::pair<LLUUID, S32> > slice;
    slice.reserve(SSSTRATA_TEX_SCAN_TICK);
    {
        LLMutexLock lock(&mHeaderMutex);
        size_map_t::const_iterator iter = mTexturesSizeMap.lower_bound(mStrataScanCursor);
        while (iter != mTexturesSizeMap.end() && slice.size() < (size_t)SSSTRATA_TEX_SCAN_TICK)
        {
            if (iter->second > 0) slice.push_back(std::make_pair(iter->first, iter->second));
            ++iter;
        }
        // Wrapping to the start rather than stopping is what makes this a rotation instead of a one-shot scan: entries written after the cursor passed them are picked up on the next lap.
        wrapped = (iter == mTexturesSizeMap.end());
        mStrataScanCursor = wrapped ? LLUUID::null : iter->first;
    }

    // The stats happen OUTSIDE the lock. They are the expensive part of this function and none of them needs the header.
    out.reserve(slice.size());
    for (size_t i = 0; i < slice.size(); ++i)
    {
        const std::string path = getTextureFileName(slice[i].first);
        llstat st;
        if (LLFile::stat(path, &st, ENOENT) != 0) continue;   // already packed, or purged underneath us; either way there is no loose file to fold
        out.push_back(SSStrataLooseFile((std::time_t)st.st_mtime, (U64)st.st_size, path, slice[i].first, false));
    }
    return wrapped;
}

// Retires the entries whose bodies died with a reclaimed volume. Without this the entry table would keep claiming a body size for a uuid whose bytes are gone, and every read of one would serve the 600 byte header record and report it as a complete image - which is a corrupt texture rather than a cache miss.
void LLTextureCache::strataDropEntries(const std::vector<LLUUID>& ids)
{
    if (ids.empty() || mReadOnly) return;

    U32 dropped = 0;
    {
        LLMutexLock lock(&mHeaderMutex);
        for (size_t i = 0; i < ids.size(); ++i)
        {
            Entry entry;
            S32 idx = openAndReadEntry(ids[i], entry, false);
            if (idx < 0) continue;

            std::string filename = getTextureFileName(ids[i]);
            removeEntry(idx, entry, filename);
            // QUEUED rather than written one at a time. writeEntryToHeaderImmediately opens and closes the entries file per record and one reclaim can drop thousands of them; mUpdatedEntryMap is the mechanism this class already has for exactly that, and it costs one open for the whole batch.
            mUpdatedEntryMap[idx] = entry;
            ++dropped;
        }
    }
    writeUpdatedEntries();

    LL_INFOS("TextureCache") << "Strata reclaim retired " << dropped << " of " << ids.size() << " texture entries whose bodies went with the volume" << LL_ENDL;
}

// <SS:Nexii> The budget arbiter's lever on this tier. Writing this one variable is the whole of doc/strata.md stage 2's J2C mechanism: the LRU purge, the startup purge and strataMaintenance below all re-read it on every pass, so the tier follows the new number at the next pass with no thread being notified of anything and nothing added to the documented mutex ordering.
//
// Entry table cost comes off here rather than at the call site, exactly as initCache takes it off max_size, so that the arbiter deals in whole tier shares and never has to know what a header entry costs.
void LLTextureCache::ssSetTexturesBudget(S64 total_bytes)
{
    // A second instance is a read-only snapshot that evicts nothing, so lowering its ceiling would only produce a log line claiming an enforcement that cannot happen.
    if (mReadOnly)
    {
        LL_INFOS("TextureCache") << "J2C budget not moved: this instance opened the texture cache read-only and purges nothing" << LL_ENDL;
        return;
    }

    const S64 entries_size = (S64)sCacheMaxEntries * (S64)(TEXTURE_CACHE_ENTRY_SIZE + TEXTURE_FAST_CACHE_ENTRY_SIZE);
    const S64 bodies       = llmax((S64)0, total_bytes - entries_size);

    // A budget of zero is a setting nobody can honour rather than an instruction to delete the cache, which is the same rule SSBC7Store::budgetBytes already applies to a zero SSBC7CacheSize. Declining loudly beats purging everything.
    if (bodies <= 0)
    {
        LL_WARNS("TextureCache") << "J2C budget not moved: a share of " << (total_bytes / (1024 * 1024))
                                 << " MB leaves nothing for bodies after " << (entries_size / (1024 * 1024))
                                 << " MB of entry table, and a zero budget is a setting we cannot honour rather than an instruction to empty the cache" << LL_ENDL;
        return;
    }

    const S64 was = sCacheMaxTexturesSize.exchange(bodies);
    if (was != bodies)
    {
        LL_INFOS("TextureCache") << "J2C budget moved from " << (was / (1024 * 1024)) << " MB to " << (bodies / (1024 * 1024))
                                 << " MB of bodies; the next purge pass enforces it" << LL_ENDL;
    }
}
// </SS:Nexii>

void LLTextureCache::strataMaintenance()
{
    SSStrataStore* strata = SSStrataStore::live(SSSTRATA_TENANT_TEXTURES);
    if (!strata) return;

    // The budget is re-read every tick rather than latched at startup, because sCacheMaxTexturesSize is deliberately movable mid-session - doc/strata.md stage 2 turns it into the arbiter's lever - and a tier enforcing a number the user changed an hour ago is worse than one enforcing none.
    const U64 budget = (U64)llmax((S64)0, sCacheMaxTexturesSize.load());
    strata->setBudgetBytes(budget);

    std::vector<SSStrataLooseFile> loose;
    const bool lap_closed = strataCollectCandidates(loose);

    const ESSStrataPackVerdict pack_verdict = strata->packPass(loose);

    U32 packed_files = 0;
    U64 loose_bytes = 0;
    for (size_t i = 0; i < loose.size(); ++i)
    {
        if (loose[i].mPacked) ++packed_files;
        else                  loose_bytes += loose[i].mSize;
    }
    // Accumulated over the lap and published only when the lap closes, because a tick sees a slice of the entry table rather than all of it. This is the number a user sees in Explorer and the one that made the asset tier look broken during a cold-cache burst, so it is worth being slow and right about rather than fast and wrong.
    mStrataLapFiles += (U32)(loose.size() - packed_files);
    mStrataLapBytes += loose_bytes;
    if (lap_closed)
    {
        strata->metrics().mLooseFiles = mStrataLapFiles;
        strata->metrics().mLooseBytes = mStrataLapBytes;
        mStrataLapFiles = 0;
        mStrataLapBytes = 0;
    }

    // Reclaim is driven by ALLOCATED bytes, not by live ones, and that distinction is the whole reason this tier needs a reclaim at all. LLTextureCache's own LRU purge already keeps the sum of live body sizes under the slider, but a packed body that is evicted frees nothing on disk until the volume holding it is unlinked. Dead bytes are therefore the only thing that can push the volumes past the budget while the live total sits comfortably under it, and killing the coldest volume is what collects them.
    //
    // 0xFFFF is passed as max_day because, unlike LLDiskCache, there is no loose-versus-packed drain race to arbitrate here: the loose tier is transient staging that the packer empties within one settle time, and the entries it belongs to are governed by the LRU purge that already exists.
    ESSStrataReclaimVerdict reclaim_verdict = SSSTRATA_RECLAIM_NOT_NEEDED;
    U32 volumes_killed = 0;
    U64 total_freed = 0;
    if (budget && strata->allocatedBytes() > budget)
    {
        std::vector<LLUUID> dropped;
        U64 freed = 0;
        reclaim_verdict = strata->reclaimColdest((U16)0xFFFF, true, freed, &dropped);
        if (reclaim_verdict == SSSTRATA_RECLAIM_RAN || reclaim_verdict == SSSTRATA_RECLAIM_UNLINK_FAILED)
        {
            // UNLINK_FAILED counts too: the records are already dead on disk, so the entries must go whether or not the file did.
            ++volumes_killed;
            total_freed += freed;
            strataDropEntries(dropped);
        }
    }

    LL_INFOS("TextureCache") << "Strata textures: pack " << ssStrataPackVerdictName(pack_verdict)
                             << ", folded " << packed_files << " of " << loose.size() << " candidates this tick"
                             << (lap_closed ? " [lap closed]" : "") << "; loose " << strata->metrics().mLooseFiles.load()
                             << " files / " << (strata->metrics().mLooseBytes.load() / (1024 * 1024))
                             << " MB at the last full lap, volumes " << (strata->allocatedBytes() / (1024 * 1024)) << " MB of "
                             << (budget / (1024 * 1024)) << " MB; reclaim " << ssStrataReclaimVerdictName(reclaim_verdict)
                             << " killed " << volumes_killed << " (" << (total_freed / (1024 * 1024)) << " MB)" << LL_ENDL;
}
// </SS:Nexii>

//////////////////////////////////////////////////////////////////////////////

LLTextureCache::ReadResponder::ReadResponder()
    : mImageSize(0),
      mImageLocal(false)
{
}

void LLTextureCache::ReadResponder::setData(U8* data, S32 datasize, S32 imagesize, S32 imageformat, bool imagelocal)
{
    if (mFormattedImage.notNull())
    {
        llassert_always(mFormattedImage->getCodec() == imageformat);
        mFormattedImage->appendData(data, datasize);
    }
    else
    {
        mFormattedImage = LLImageFormatted::createFromType(imageformat);
        mFormattedImage->setData(data,datasize);
    }
    mImageSize = imagesize;
    mImageLocal = imagelocal;
}

//////////////////////////////////////////////////////////////////////////////
