/**
 * @file llprocessor.h
 * @brief Code to figure out the processor. Originally by Benjamin Jurke.
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
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


#ifndef LLPROCESSOR_H
#define LLPROCESSOR_H
#include "llunits.h"
#include "llpreprocessor.h"

class LLProcessorInfoImpl;

// <SS:Nexii> Baseline instruction set check.
//
// These are deliberately free functions built on raw __cpuid rather than members of LLProcessorInfo, because LLProcessorInfo answers its hasSSE*() queries by string lookup into a per-platform map that is populated differently on Windows, Linux and macOS, and constructing it measures the CPU frequency by busy-waiting 50ms at realtime priority. The startup guard has to run before any of that, cannot afford the delay, and must not depend on the map having been populated correctly.
//
// AVX support cannot be read from the CPUID feature bit alone: the OS must also have enabled YMM state save/restore, which is why hasAVX() checks OSXSAVE and XGETBV as well. A CPU with the AVX bit set under an OS that never enabled the state will fault on the first VEX-256 instruction.
namespace LLCPUFeatures
{
    LL_COMMON_API bool hasAVX();
    LL_COMMON_API bool hasAVX2();
    LL_COMMON_API bool hasFMA3();

    // The instruction set this binary was actually compiled for, driven by the USE_AVX*_OPTIMIZATION defines that indra/cmake/00-Common.cmake turns into /arch flags. Returns a short literal such as "AVX2" or "SSE2".
    LL_COMMON_API const char* buildTargetISA();

    // False when the running CPU cannot execute the instruction set this binary was built for. Callers are expected to tell the user and exit rather than continue into an illegal instruction fault with no diagnostic.
    LL_COMMON_API bool runningCPUSupportsBuildTarget();
}
// </SS:Nexii>

class LL_COMMON_API LLProcessorInfo
{
public:
    LLProcessorInfo();
    ~LLProcessorInfo();

    F64MegahertzImplicit getCPUFrequency() const;
    bool hasSSE() const;
    bool hasSSE2() const;
    bool hasSSE3() const;
    bool hasSSE3S() const;
    bool hasSSE41() const;
    bool hasSSE42() const;
    bool hasSSE4a() const;
    bool hasAVX() const;
    bool hasAVX2() const;
    bool hasAVX512F() const;
    bool hasAltivec() const;
    std::string getCPUFamilyName() const;
    std::string getCPUBrandName() const;
    std::string getCPUFeatureDescription() const;
private:
    LLProcessorInfoImpl* mImpl;
};

#endif // LLPROCESSOR_H
