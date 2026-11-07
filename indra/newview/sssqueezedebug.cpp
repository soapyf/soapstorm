/**
 * @file sssqueezedebug.cpp
 * @brief Squeeze P0 self test - synthetic BC7 upload and VRAM accounting proof, see doc/viewer/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "sssqueezedebug.h"

#include "llgl.h"
#include "llimage.h"
#include "llimagegl.h"
#include "llmath.h"
#include "llrender.h"
#include "llstring.h"
#include "llviewercontrol.h"
#include "ssbc7adaptive.h"
#include "ssbc7encodequeue.h"
#include "ssbc7manifest.h"
#include "ssbc7promote.h"
#include "ssbc7serve.h"

#include <cstring>
#include <vector>

namespace
{
    // 64x64 yields a six level chain of 64,32,16,8,4,2 under MAX_DISCARD_LEVEL, which includes the sub-4x4 tail that exercises the block size clamp in dataFormatBytes
    const S32 SS_SQUEEZE_TEST_SIZE = 64;
    const S32 SS_BC7_BLOCK_BYTES = 16;

    void ss_bc7_put_bits(U8* block, U32& bit, U32 value, U32 count)
    {
        for (U32 i = 0; i < count; ++i)
        {
            if ((value >> i) & 1u)
            {
                block[bit >> 3] = (U8)(block[bit >> 3] | (1u << (bit & 7u)));
            }
            ++bit;
        }
    }

    // One BC7 mode 6 block with both endpoints set to the same colour and all 63 index bits left at zero, so every texel of the 4x4 decodes to exactly that colour
    void ss_bc7_solid_block(U8* block, U8 r, U8 g, U8 b, U8 a)
    {
        memset(block, 0, SS_BC7_BLOCK_BYTES);
        U32 bit = 0;
        ss_bc7_put_bits(block, bit, 1u << 6, 7);        // mode 6 is unary coded: six zero bits then a one
        ss_bc7_put_bits(block, bit, (U32)(r >> 1), 7);  // R0
        ss_bc7_put_bits(block, bit, (U32)(r >> 1), 7);  // R1
        ss_bc7_put_bits(block, bit, (U32)(g >> 1), 7);  // G0
        ss_bc7_put_bits(block, bit, (U32)(g >> 1), 7);  // G1
        ss_bc7_put_bits(block, bit, (U32)(b >> 1), 7);  // B0
        ss_bc7_put_bits(block, bit, (U32)(b >> 1), 7);  // B1
        ss_bc7_put_bits(block, bit, (U32)(a >> 1), 7);  // A0
        ss_bc7_put_bits(block, bit, (U32)(a >> 1), 7);  // A1
        ss_bc7_put_bits(block, bit, 1u, 1);             // P0 - endpoints are (value << 1) | pbit, so 127 with a set pbit is exactly 255
        ss_bc7_put_bits(block, bit, 1u, 1);             // P1
    }

    U32 ss_drain_gl_errors()
    {
        U32 last = GL_NO_ERROR;
        for (S32 i = 0; i < 16; ++i)
        {
            U32 err = glGetError();
            if (err == GL_NO_ERROR)
            {
                break;
            }
            last = err;
        }
        return last;
    }
}

void ss_squeeze_refresh_enabled()
{
    LLImageGL::sSqueezeEnabled = gSavedSettings.getBOOL("SSSqueezeEnabled");
    LL_INFOS("Squeeze") << "SSSqueezeEnabled is now " << (LLImageGL::sSqueezeEnabled ? "on" : "off")
        << ", BC7 uploads " << (LLImageGL::canUseSqueeze() ? "permitted" : "gated off") << LL_ENDL;

    // The encode side keeps its own snapshot of the same settings, because it is read from the texture fetch thread and that thread has no business touching the control group. Refreshing both from one place is what stops the upload gate and the encode gate from ever disagreeing.
    ssBC7EncodeRefreshPolicy();

    // <SS:Nexii> Squeeze - and the read side, for the same reason: the read gate and the encode gate must come from ONE function or a session can end up encoding what it will not serve, or serving from a store nothing is filling.
    ssBC7ServeRefreshPolicy();

    // <SS:Nexii> Squeeze promotion - and the fill side, for exactly the same reason. SSSqueezeNetworkPromote is read here and nowhere else, so this is the call that makes the preferences checkbox mean something rather than decorate the panel.
    ssBC7PromoteRefreshPolicy();

    // <SS:Nexii/> Squeeze adaptive quality - and the controller, so SSSqueezeEncodeQuality can be pinned or handed back to the adaptive ladder mid-session. It used to need a restart because a different profile meant a different encoder version and therefore a wiped store; with the profile in the record, changing it now costs nothing already written.
    ssBC7AdaptiveRefreshPolicy();

    // <SS:Nexii/> Squeeze region manifests - and the pre-warm, for the same reason again: the recorder and the read gate must agree about whether the feature is on, or a session can spend a walk recording uuids into a manifest nothing will ever be allowed to serve.
    ssBC7ManifestRefreshPolicy();
    // </SS:Nexii>
    // </SS:Nexii>
}

void ss_squeeze_self_test()
{
    if (!gGLManager.mInited || gGLManager.mIsDisabled)
    {
        LL_WARNS("Squeeze") << "self test skipped, GL is not available" << LL_ENDL;
        return;
    }

    LL_INFOS("Squeeze") << "GL " << gGLManager.mGLVersion
        << ", mHasBPTC " << (gGLManager.mHasBPTC ? 1 : 0)
        << ", SSSqueezeEnabled " << (LLImageGL::sSqueezeEnabled ? 1 : 0)
        << ", canUseSqueeze " << (LLImageGL::canUseSqueeze() ? 1 : 0) << LL_ENDL;

    if (!LLImageGL::canUseSqueeze())
    {
        LL_INFOS("Squeeze") << "self test skipped, BC7 is gated off and the uncompressed path stays in use" << LL_ENDL;
        return;
    }

    gGL.getTexUnit(0)->activate();
    ss_drain_gl_errors();

    // allow_compression false so RenderCompressTextures cannot rewrite the internal format underneath the uncompressed leg of this test
    LLPointer<LLImageGL> image = new LLImageGL(true, false);
    if (!image->setSize(SS_SQUEEZE_TEST_SIZE, SS_SQUEEZE_TEST_SIZE, 4))
    {
        LL_WARNS("Squeeze") << "self test failed, setSize rejected the test dimensions" << LL_ENDL;
        return;
    }
    image->setExplicitFormat(GL_COMPRESSED_RGBA_BPTC_UNORM, GL_COMPRESSED_RGBA_BPTC_UNORM);

    const S32 max_discard = image->getMaxDiscardLevel();
    S64 total_bytes = 0;
    for (S32 d = 0; d <= max_discard; ++d)
    {
        total_bytes += LLImageGL::dataFormatBytes(GL_COMPRESSED_RGBA_BPTC_UNORM, image->getWidth(d), image->getHeight(d));
    }
    const S64 base_bytes = LLImageGL::dataFormatBytes(GL_COMPRESSED_RGBA_BPTC_UNORM, SS_SQUEEZE_TEST_SIZE, SS_SQUEEZE_TEST_SIZE);

    // setImage walks data_in BACKWARD one mip at a time, so the blob is laid out smallest mip first and the pointer handed to createGLTexture points at the start of the largest
    std::vector<U8> blob((size_t)total_bytes, 0);
    static const U8 s_mip_colours[6][3] = { {255, 32, 32}, {32, 255, 32}, {64, 64, 255}, {255, 255, 32}, {255, 32, 255}, {32, 255, 255} };
    S64 running = 0;
    for (S32 d = 0; d <= max_discard; ++d)
    {
        const S64 level_bytes = LLImageGL::dataFormatBytes(GL_COMPRESSED_RGBA_BPTC_UNORM, image->getWidth(d), image->getHeight(d));
        running += level_bytes;
        U8* dst = blob.data() + (total_bytes - running);
        U8 block[SS_BC7_BLOCK_BYTES];
        const U8* colour = s_mip_colours[llmin(d, 5)];
        ss_bc7_solid_block(block, colour[0], colour[1], colour[2], 255);
        for (S64 off = 0; off + SS_BC7_BLOCK_BYTES <= level_bytes; off += SS_BC7_BLOCK_BYTES)
        {
            memcpy(dst + off, block, SS_BC7_BLOCK_BYTES);
        }
    }
    const U8* largest = blob.data() + (total_bytes - base_bytes);

    // phase A - first upload, brand new texture name
    U64 before = LLImageGL::getTextureBytesAllocated();
    bool ok = image->createGLTexture(0, largest, true);
    U32 err = ss_drain_gl_errors();
    S64 delta = (S64)LLImageGL::getTextureBytesAllocated() - (S64)before;
    LL_INFOS("Squeeze") << "phase A first BC7 upload " << (ok ? "ok" : "FAILED")
        << ", tex " << image->getTexName()
        << ", mips 0.." << max_discard
        << ", blob " << total_bytes << " bytes"
        << ", accounted " << delta << " expected " << base_bytes
        << ", gl error " << llformat("0x%04x", err)
        << ((ok && delta == base_bytes && err == GL_NO_ERROR) ? " PASS" : " FAIL") << LL_ENDL;

    // phase B - re-upload into the SAME texture name, the path that needs the explicit free before the explicit alloc
    before = LLImageGL::getTextureBytesAllocated();
    ok = image->createGLTexture(0, largest, true);
    err = ss_drain_gl_errors();
    delta = (S64)LLImageGL::getTextureBytesAllocated() - (S64)before;
    LL_INFOS("Squeeze") << "phase B BC7 re-upload into the same texture " << (ok ? "ok" : "FAILED")
        << ", accounted " << delta << " expected 0"
        << ", gl error " << llformat("0x%04x", err)
        << ((ok && delta == 0 && err == GL_NO_ERROR) ? " PASS" : " FAIL") << LL_ENDL;

    // phase C - replace the BC7 resident with an uncompressed image, the explicit format lifecycle hazard
    LLPointer<LLImageRaw> raw = new LLImageRaw((U16)SS_SQUEEZE_TEST_SIZE, (U16)SS_SQUEEZE_TEST_SIZE, 4);
    memset(raw->getData(), 0x80, (size_t)raw->getDataSize());

    const S64 raw_expect = LLImageGL::dataFormatBytes(GL_RGBA8, SS_SQUEEZE_TEST_SIZE, SS_SQUEEZE_TEST_SIZE) - base_bytes;
    before = LLImageGL::getTextureBytesAllocated();
    ok = image->createGLTexture(0, raw.get());
    err = ss_drain_gl_errors();
    delta = (S64)LLImageGL::getTextureBytesAllocated() - (S64)before;
    LL_INFOS("Squeeze") << "phase C uncompressed replace " << (ok ? "ok" : "FAILED")
        << ", primary format now " << llformat("0x%04x", (U32)image->getPrimaryFormat())
        << ", has explicit format " << (image->getHasExplicitFormat() ? 1 : 0)
        << ", accounted " << delta << " expected " << raw_expect
        << ", gl error " << llformat("0x%04x", err)
        << ((ok && !image->getHasExplicitFormat() && delta == raw_expect && err == GL_NO_ERROR) ? " PASS" : " FAIL") << LL_ENDL;

    LL_INFOS("Squeeze") << "running total " << LLImageGL::getTextureBytesAllocated()
        << " bytes; the test texture is released a few frames later by LLImageGL::updateClass" << LL_ENDL;
}
