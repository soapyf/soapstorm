/**
 * @file ssglreadback.cpp
 * @brief See ssglreadback.h.
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

#include "ssglreadback.h"

#include "llrender.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "threadpool.h"
#include "workqueue.h"

#include <thread>
#include <vector>

// <SS:Nexii> The thread behind SSGLReadback: one worker with its own GL context shared with the viewer's, lifecycle mirroring LLImageGLThread and SSWindFlowMap's SSWindFlowGLWorker (createSharedContext -> makeContextCurrent -> gGL.init -> service queue -> gGL.shutdown -> destroySharedContext). The body uses only raw GL - never gGL, LLVertexBuffer or the bound-shader statics - so it cannot disturb the main thread's GL state. mStarted flips the instant the thread services its queue; createWorker waits on it so a worker whose GL setup failed is caught immediately and never trusted with jobs.
class SSGLReadback::Worker : public LL::ThreadPool
{
public:
    Worker(const std::string& name, LLWindow* window, void* context,
           std::atomic<bool>* started)
        : LL::ThreadPool(name, 1)
        , mWindow(window)
        , mContext(context)
        , mStarted(started)
    {
    }

    void run() override
    {
        try
        {
            mWindow->makeContextCurrent(mContext);
            gGL.init(false);
            LL_PROFILER_GPU_CONTEXT_NS("SSGLReadback Context", 17);
        }
        catch (...)
        {
            LL_WARNS("AtmoMagic") << "SSGLReadback: worker GL setup failed - "
                                     "readbacks stay on the main thread" << LL_ENDL;
            return;
        }

        mStarted->store(true, std::memory_order_release);
        LL::ThreadPool::run();
        gGL.shutdown();
        mWindow->destroySharedContext(mContext);
        mContext = nullptr;
    }

private:
    LLWindow* mWindow;
    void* mContext;
    std::atomic<bool>* mStarted;
};

SSGLReadback::SSGLReadback()
{
}

SSGLReadback::~SSGLReadback()
{
    shutdown();
}

void SSGLReadback::shutdown()
{
    mPending.clear();
    mWorker.reset();
    mAvailable = false;
}

// <SS:Nexii> Lazily creates the worker on first submit. Only a platform that can hand over a context shared with the viewer's counts (Windows/SDL shared contexts; not headless); without one every read stays on the calling thread - exactly today's behaviour. Waits briefly for the thread to reach its queue (see Worker::run) so a failing context is caught at creation, not by a stranded job.
bool SSGLReadback::createWorker()
{
    static LLCachedControl<bool> worker_setting(gSavedSettings, "SSGLReadbackWorker", true);
    if (!worker_setting) return false;
    if (mTried) return mAvailable && !mDisarmed;
    mTried = true;

    if (!gViewerWindow) return false;

    LLWindow* window = gViewerWindow->getWindow();
    if (!window) return false;

    void* context = window->createSharedContext();
    if (!context)
    {
        LL_WARNS("AtmoMagic") << "SSGLReadback: no shared GL context - texture "
                                 "readbacks stay on the main thread" << LL_ENDL;
        return false;
    }

    mStarted = false;
    mWorker = std::make_unique<Worker>("SSGLReadback", window, context, &mStarted);
    mWorker->start();

    // One-time spin-up payoff: if the worker cannot get a working GL
    // context on this machine, find out now (a few ms) instead of after a
    // round of silently unanswered jobs.
    for (int i = 0; i < 50 && !mStarted.load(std::memory_order_acquire); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    if (!mStarted.load(std::memory_order_acquire))
    {
        LL_WARNS("AtmoMagic") << "SSGLReadback: worker thread failed to start - "
                                 "texture readbacks stay on the main thread" << LL_ENDL;
        mWorker.reset();
        mStarted = false;
        return false;
    }

    mAvailable = true;

    LL_INFOS("AtmoMagic") << "SSGLReadback: texture readbacks now run on a "
                             "dedicated GL worker thread behind the frame loop" << LL_ENDL;
    return true;
}

bool SSGLReadback::available()
{
    return createWorker();
}

// Bytes per texel. Only the combinations the atmo systems actually read are
// enumerated; unknown formats default to the widest (RGBA, 4).
S32 SSGLReadback::components(GLenum format, GLenum type)
{
    S32 channels;
    switch (format)
    {
        case GL_RED:
        case GL_ALPHA:
        case GL_LUMINANCE:
        case GL_DEPTH_COMPONENT:
            channels = 1;
            break;
        case GL_RG:
        case GL_LUMINANCE_ALPHA:
            channels = 2;
            break;
        case GL_RGB:
        case GL_BGR:
            channels = 3;
            break;
        case GL_RGBA:
        case GL_BGRA:
        default:
            channels = 4;
            break;
    }

    S32 bytes;
    switch (type)
    {
        case GL_FLOAT:
        case GL_INT:
        case GL_UNSIGNED_INT:
            bytes = 4;
            break;
        case GL_HALF_FLOAT:
        case GL_SHORT:
        case GL_UNSIGNED_SHORT:
            bytes = 2;
            break;
        case GL_UNSIGNED_BYTE:
        case GL_BYTE:
        default:
            bytes = 1;
            break;
    }

    return channels * bytes;
}

// <SS:Nexii> The actual read. glGetTexImage reads from whatever is bound to the target on the active texture unit - the texture name is not a parameter - so the job's texture must be bound around the call. Missing that bind is silent: GL_INVALID_OPERATION, nothing written, and the caller is left holding the zero-filled buffer it allocated, which a depth consumer reads as "every column hit at the near plane". The previous binding is restored because this also runs inline on the main thread, where LLTexUnit caches what it thinks is bound; leaving ours there would desync that cache for whoever draws next.
void SSGLReadback::readTexture(const Job& job, U8* dest)
{
    const GLenum binding = (job.mTarget == GL_TEXTURE_3D) ? GL_TEXTURE_BINDING_3D
                         : (job.mTarget == GL_TEXTURE_CUBE_MAP) ? GL_TEXTURE_BINDING_CUBE_MAP
                         : GL_TEXTURE_BINDING_2D;

    GLint old_texture = 0;
    glGetIntegerv(binding, &old_texture);

    GLint old_pack = 0;
    glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack);

    glBindTexture(job.mTarget, job.mTexture);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    glGetTexImage(job.mTarget, job.mLevel, job.mFormat, job.mType, dest);

    glPixelStorei(GL_PACK_ALIGNMENT, old_pack);
    glBindTexture(job.mTarget, (GLuint)old_texture);
}

// Synchronous fallback: same read on the calling thread. Rows are tightly
// packed so the layout matches what the worker produces.
bool SSGLReadback::runInline(const Job& job)
{
    std::vector<U8> buffer((size_t)job.mWidth * job.mHeight * job.mDepth * components(job.mFormat, job.mType), 0);

    readTexture(job, buffer.data());

    if (job.mConvert) job.mConvert(buffer.data(), buffer.size());
    if (job.mDone) job.mDone(buffer.data(), buffer.size());
    return true;
}

// Delivers a job's mDone on the main thread, exactly once, whichever path got
// the bytes there (worker read, inline fallback). Only ever called from the
// main thread: poll() and the mainloop-completion lambda both run there.
void SSGLReadback::finish(const std::shared_ptr<PendingJob>& job)
{
    if (job->mFinished.exchange(true)) return;
    if (job->mJob.mDone) job->mJob.mDone(job->mBuffer->data(), job->mBuffer->size());
    job->mBuffer.reset();
}

bool SSGLReadback::submit(const Job& job)
{
    if (job.mTexture == 0 || job.mWidth <= 0 || job.mHeight <= 0 || job.mDepth <= 0)
    {
        return false;
    }

    if (!createWorker())
    {
        return runInline(job);
    }

    // The worker's context shares objects with ours; order the read after the
    // caller's render by fencing this context and flushing, so the read never
    // sees a half-written texture.
    GLsync write_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();

    std::shared_ptr<PendingJob> pending = std::make_shared<PendingJob>();
    pending->mJob = job;
    pending->mBuffer = std::make_shared<std::vector<U8>>(
        (size_t)job.mWidth * job.mHeight * job.mDepth * components(job.mFormat, job.mType));
    pending->mSubmitted = std::chrono::steady_clock::now();
    mPending.push_back(pending);

    LL::WorkQueue::ptr_t main = LL::WorkQueue::getInstance("mainloop");
    const bool posted = mWorker->getQueue().post([pending, write_fence, main]()
    {
        if (write_fence)
        {
            glWaitSync(write_fence, 0, GL_TIMEOUT_IGNORED);
            glFlush();
            glDeleteSync(write_fence);
        }

        const Job& job = pending->mJob;
        SSGLReadback::readTexture(job, pending->mBuffer->data());

        if (job.mConvert) job.mConvert(pending->mBuffer->data(), pending->mBuffer->size());
        pending->mReadDone.store(true, std::memory_order_release);

        // Low-latency delivery; poll() is the net that catches a worker the
        // completion post could never reach.
        if (main)
        {
            main->post([pending]()
            {
                SSGLReadback::getInstance()->finish(pending);
            });
        }
    });

    if (!posted)
    {
        mPending.remove(pending);
        return runInline(job);
    }
    return true;
}

// Main thread, once a frame. Drains worker-completed jobs whose mainloop
// delivery raced ahead of us, and - after a generous holdout - reads unserviced
// jobs inline and disarms the worker so nothing stays stranded. The inline
// read races a hypothetical mid-read worker, but the holdout only trips when
// the worker is dead or hung, and the read is the same texture, same layout.
void SSGLReadback::poll()
{
    static const std::chrono::milliseconds HOLDOUT(1000);

    if (mPending.empty()) return;
    const auto now = std::chrono::steady_clock::now();

    for (auto it = mPending.begin(); it != mPending.end();)
    {
        const std::shared_ptr<PendingJob>& pending = *it;
        const bool overdue = (now - pending->mSubmitted) > HOLDOUT;

        if (pending->mReadDone.load(std::memory_order_acquire))
        {
            finish(pending);
            it = mPending.erase(it);
            continue;
        }

        if (overdue)
        {
            LL_WARNS("AtmoMagic") << "SSGLReadback: worker readback never "
                                     "completed - falling back to the main "
                                     "thread for this and future reads" << LL_ENDL;
            mDisarmed = true;
            mAvailable = false;

            const Job& job = pending->mJob;
            readTexture(job, pending->mBuffer->data());
            if (job.mConvert) job.mConvert(pending->mBuffer->data(), pending->mBuffer->size());
            pending->mReadDone.store(true, std::memory_order_release);

            finish(pending);
            it = mPending.erase(it);
            continue;
        }

        ++it;
    }
}