/**
 * @file ssdiscpad.h
 * @brief Atmo Magic: auto-derives a celestial body's disc padding from its texture.
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

#ifndef SS_DISCPAD_H
#define SS_DISCPAD_H

#include "llsd.h"

class LLUUID;

// <SS:Nexii> The outcome of a disc-padding analysis. OK and FAILED are both final - the caller's texture pixels were read - and FAILED means the lines could not agree where the disc is, erroring out to the 0 full-bleed padding. LOADING means the texture's decoded data has not arrived yet; the request is re-checked by ssDiscPadPoll().
enum class SSDiscPadStatus
{
    OK,
    LOADING,
    FAILED
};

// Single-shot analysis of a disc texture's alpha, without touching the asset: reads the texture's decoded pixels (kick-starting the raw-image readback like the cloud deck does) and walks slightly rotated cardinal and diagonal lines out from the centre until each leaves the disc, taking the disc fraction the majority of the sixteen radial samples agree on. out_padding receives each side's transparent margin as a fraction of the texture width; returns LOADING when the pixels are not decoded yet - out_padding is left untouched then.
SSDiscPadStatus ssDiscPadAnalyze(const LLUUID& texture_id, F32& out_padding);

// Derives and APPLIES the disc padding of a live asset body whose disc texture just changed (a texture pick or a sky import) - the padding is the auto part; the asset carries on authoring it. Writes the derived padding to the body, or 0 when the lines cannot agree (the error-out), and remembers any still-loading derivations so ssDiscPadPoll() can retry them. Gated on SSAtmoDiscPadAuto: off, nothing is touched.
void ssDiscPadAutoDerive(S32 track_index, S32 body_index, const LLUUID& texture_id);

// Retries the still-loading derivations against the live asset. Tick from any UI poll at a sub-second cadence while the environment UI is up (the environment and planetary floaters both do). A derivation whose body is gone, whose texture was changed again, or whose texture never arrives times out and errors out to 0 padding.
void ssDiscPadPoll();

#endif