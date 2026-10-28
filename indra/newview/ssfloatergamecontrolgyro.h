/**
 * @file ssfloatergamecontrolgyro.h
 * @brief Gyro aim settings, with a live readout and calibration.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * SkoomaStorm Viewer Source Code
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
 * $/LicenseInfo$
 */

#ifndef SS_FLOATERGAMECONTROLGYRO_H
#define SS_FLOATERGAMECONTROLGYRO_H

#include "llfloater.h"

class LLTextBox;

// Gyro aim for pads that have one: turning the pad turns the view in mouselook, like a
// mouse. Every change applies and saves at once; the readout shows the pad's rotation
// rates so calibration and the signs can be checked.
class SSFloaterGameControlGyro : public LLFloater
{
public:
    SSFloaterGameControlGyro(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    void refresh() override;
    void onCommit();
    void onPadsCommit(); // <GMDX> trackpads

    LLTextBox* mStatus { nullptr };
    LLTextBox* mReadout { nullptr };
    LLTextBox* mPadsStatus { nullptr };  // <GMDX>
    LLTextBox* mPadsReadout { nullptr };
};

#endif // SS_FLOATERGAMECONTROLGYRO_H
