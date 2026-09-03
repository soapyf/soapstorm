/**
 * @file ssglreadback.h
 * @brief Soapstorm: a shared GL context reading textures back to the CPU off
 *        the main thread.
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

#ifndef SS_GLREADBACK_H
#define SS_GLREADBACK_H

#include "llrendertarget.h"
#include "llsingleton.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <list>
#include <memory>
#include <vector>

// <SS:Nexii> A generic texture->CPU readback worker. One dedicated thread with its own GL context shared with the viewer's (createSharedContext -> makeContextCurrent -> gGL.init, the LLImageGLThread lifecycle), so readbacks that would otherwise stall the frame loop - glGetTexImage of depth maps, volume data, baked textures - run behind it. The caller renders into the source texture on the main thread and submits a Job; the worker waits on a fence (never reading a half-written texture), copies the texels into a buffer, and posts the result back to the mainloop queue. The worker touches no viewer GL state: the read uses raw GL only, so it cannot mutate the main thread's GL state machine (gGL, LLVertexBuffer, the bound-shader statics). The generic form of the pattern SSWindFlowMap's SSWindFlowGLWorker proved out, so the atmo systems that read textures (rain shadow, world field bands, wind flow) share one worker instead of each building one. Contract: the source texture must stay allocated and unre-rendered, unreallocated until the Job's completion callback runs. Capture pipelines that reuse one render target (rain shadows, world field bands) serialize by not starting the next render while a readback is outstanding.
class SSGLReadback : public LLSingleton<SSGLReadback>
{
    // Ctor and dtor are out of line, in the .cpp: mWorker is a unique_ptr to the
    // forward-declared Worker, and an inline ctor would make every translation
    // unit including this header instantiate the unique_ptr destructor against
    // an incomplete type.
    LLSINGLETON(SSGLReadback);
    ~SSGLReadback() override;

public:
    struct Job
    {
        U32 mTexture = 0;                  // GL texture name, shared with the main context
        GLenum mTarget = GL_TEXTURE_2D;    // GL_TEXTURE_2D or GL_TEXTURE_3D
        GLint mLevel = 0;                  // mip level to read
        S32 mWidth = 0;
        S32 mHeight = 0;
        S32 mDepth = 1;                    // slices for 3D textures
        GLenum mFormat = GL_RGBA;
        GLenum mType = GL_UNSIGNED_BYTE;

        // Runs on the readback worker after the read, with the tightly packed
        // texels - CPU transforms that should not run on the main thread.
        // Optional.
        std::function<void(const U8* data, size_t bytes)> mConvert;
        // Runs on the main thread with the packed texels once the read (and
        // any convert) has completed; always called, worker or inline. Copy
        // the bytes into your own store here - the backing buffer is released
        // when this callback returns.
        std::function<void(const U8* data, size_t bytes)> mDone;
    };

    // Submit a texture readback. Runs on the dedicated worker when one can be
    // had (SSGLReadbackWorker setting, shared context available), synchronously
    // on the calling thread otherwise; either way mDone fires exactly once. A
    // job the worker never completes is finished inline after a short holdout
    // by poll() (see below), so a dead worker can never strand a capture.
    bool submit(const Job& job);

    // True when a shared-context worker is (or can be) running.
    bool available();

    // Main thread, once a frame: completes jobs the worker has finished (the
    // low-latency path posts to the mainloop queue) and, if a job has gone
    // unserviced too long, reads it inline and disarms the worker so every
    // later job stays on the calling thread.
    void poll();

    // Bytes per texel for the format/type pair - the size submit() reads.
    static S32 components(GLenum format, GLenum type);

    // Binds the job's texture, reads it tightly packed into dest, and restores
    // the previous binding. Every read path goes through here.
    static void readTexture(const Job& job, U8* dest);

private:
    struct PendingJob
    {
        Job mJob;
        std::shared_ptr<std::vector<U8>> mBuffer;
        std::chrono::steady_clock::time_point mSubmitted;
        std::atomic<bool> mReadDone{ false };    // worker (or inline fallback) filled mBuffer
        std::atomic<bool> mFinished{ false };    // mDone has run; exactly-once guard
    };

    // Runs the (shared) threaded read for a pending job, then delivers mDone
    // on the main thread exactly once. Inline fallback and late worker
    // completion both round-trip through here.
    void finish(const std::shared_ptr<PendingJob>& job);

    void shutdown();

    class Worker;

    bool createWorker();
    bool runInline(const Job& job);

    std::unique_ptr<Worker> mWorker;
    std::list<std::shared_ptr<PendingJob>> mPending;
    std::atomic<bool> mStarted{ false };   // the worker reached its run loop; success handshake
    bool mTried = false;
    bool mAvailable = false;
    bool mDisarmed = false;
};


#endif