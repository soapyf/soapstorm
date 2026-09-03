/**
 * @file ssatmoenvkeyframe.h
 * @brief Atmo Magic: per-parameter keyframe container.
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

#ifndef SS_ATMOENVKEYFRAME_H
#define SS_ATMOENVKEYFRAME_H

#include "llsd.h"
#include "llsdutil.h"
#include "lluuid.h"
#include "v2math.h"
#include "v3color.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

enum class SSAtmoEnvCurve : U8
{
    EASE   = 0,
    LINEAR = 1,
    HOLD   = 2
};

inline std::string ss_atmoenv_curve_name(SSAtmoEnvCurve c)
{
    switch (c)
    {
        case SSAtmoEnvCurve::LINEAR: return "linear";
        case SSAtmoEnvCurve::HOLD:   return "hold";
        default:                    return "ease";
    }
}

inline SSAtmoEnvCurve ss_atmoenv_curve_from_name(const std::string& name)
{
    if (name == "linear") return SSAtmoEnvCurve::LINEAR;
    if (name == "hold")   return SSAtmoEnvCurve::HOLD;
    return SSAtmoEnvCurve::EASE;
}

template <typename T>
struct SSAtmoEnvKeyframe
{
    F64 mTime = 0.0;
    T mValue{};
    SSAtmoEnvCurve mCurve = SSAtmoEnvCurve::EASE;
};

template <typename T>
inline T ss_atmoenv_lerp(const T& a, const T& b, F32 t)
{
    return (T)(a + (b - a) * t);
}

template <>
inline std::string ss_atmoenv_lerp<std::string>(const std::string& a, const std::string& /*b*/, F32 /*t*/)
{
    return a;
}

// <SS:Nexii> The guard rail behind a flag's HOLD curve rather than the rule flags actually follow. Every bool keyframe is HOLD (see ss_atmoenv_default_curve<bool> below), and a HOLD segment weighs everything onto its earlier keyframe, so this is only ever asked for t = 0. It stays specialised because the generic lerp on a bool is an arithmetic cast - (a + (b - a) * t) reads true for all but the first instant of a false->true segment - and a midpoint step is the least surprising thing to fall back to if a curve ever reaches here that is not a HOLD.
template <>
inline bool ss_atmoenv_lerp<bool>(const bool& a, const bool& b, F32 t)
{
    return t < 0.5f ? a : b;
}

template <typename T>
inline SSAtmoEnvCurve ss_atmoenv_default_curve() { return SSAtmoEnvCurve::EASE; }

template <>
inline SSAtmoEnvCurve ss_atmoenv_default_curve<std::string>() { return SSAtmoEnvCurve::HOLD; }

// <SS:Nexii> A flag has nothing to interpolate, so HOLD is the only curve that means anything on one: the value stands from its own keyframe until the next, which is what the checkbox in front of the author says it does - ticked here, and off again where it unticks. EASE on a bool is not a curve at all, it is a step at the segment MIDPOINT, so the rain used to start halfway between the keyframe that turned it on and whatever key happened to precede it - a boundary in a place the author never put a mark. Same reasoning as the string specialisation above; a precipitation TYPE cannot be half snow either.
template <>
inline SSAtmoEnvCurve ss_atmoenv_default_curve<bool>() { return SSAtmoEnvCurve::HOLD; }

// <SS:Nexii> What a stored curve becomes when a document is read back. Normally itself - a curve is authored intent and survives a round trip. Bool is the exception: EASE on a flag was never intent, it was the default curve landing on a type that has no use for one, so documents written before flags went to HOLD carry it and would keep their midpoint steps forever. There is nothing to preserve, so it is corrected on the way in rather than migrated.
template <typename T>
inline SSAtmoEnvCurve ss_atmoenv_curve_on_load(SSAtmoEnvCurve stored) { return stored; }

template <>
inline SSAtmoEnvCurve ss_atmoenv_curve_on_load<bool>(SSAtmoEnvCurve /*stored*/) { return SSAtmoEnvCurve::HOLD; }

template <>
inline LLUUID ss_atmoenv_lerp<LLUUID>(const LLUUID& a, const LLUUID& /*b*/, F32 /*t*/)
{
    return a;
}

// <SS:Nexii> Texture tracks keyframe at EASE, not HOLD. ss_atmoenv_lerp never interpolates a UUID - valueAt holds the earlier map, the crossfade's FROM keyframe - so the curve's only render role is the weight blendAt() hands the renderer, and HOLD weighed that to 0 on every segment: no curve control on a texture row meant the crossfade never engaged and every cloud image change snapped. HOLD stays expressible for an authored hard cut (setCurveAt, or an asset carrying "hold").
template <>
inline SSAtmoEnvCurve ss_atmoenv_default_curve<LLUUID>() { return SSAtmoEnvCurve::EASE; }

template <typename T>
inline bool ss_atmoenv_near_equal(const T& a, const T& b, F32 epsilon)
{
    return llabs((F64)(a - b)) <= (F64)epsilon;
}

template <>
inline bool ss_atmoenv_near_equal<LLColor3>(const LLColor3& a, const LLColor3& b, F32 epsilon)
{
    return llabs(a.mV[0] - b.mV[0]) <= epsilon
        && llabs(a.mV[1] - b.mV[1]) <= epsilon
        && llabs(a.mV[2] - b.mV[2]) <= epsilon;
}

template <>
inline bool ss_atmoenv_near_equal<LLVector2>(const LLVector2& a, const LLVector2& b, F32 epsilon)
{
    return llabs(a.mV[0] - b.mV[0]) <= epsilon
        && llabs(a.mV[1] - b.mV[1]) <= epsilon;
}

template <>
inline bool ss_atmoenv_near_equal<LLUUID>(const LLUUID& a, const LLUUID& b, F32 /*epsilon*/)
{
    return a == b;
}

template <>
inline bool ss_atmoenv_near_equal<std::string>(const std::string& a, const std::string& b, F32 /*epsilon*/)
{
    return a == b;
}

template <typename T> LLSD ss_atmoenv_value_to_sd(const T& v);
template <typename T> T ss_atmoenv_value_from_sd(const LLSD& sd, const T& fallback);

template <> inline LLSD ss_atmoenv_value_to_sd<F32>(const F32& v) { return (LLSD::Real)v; }
template <> inline F32 ss_atmoenv_value_from_sd<F32>(const LLSD& sd, const F32& fallback)
{
    return sd.isReal() || sd.isInteger() ? (F32)sd.asReal() : fallback;
}

template <> inline LLSD ss_atmoenv_value_to_sd<std::string>(const std::string& v) { return v; }
template <> inline std::string ss_atmoenv_value_from_sd<std::string>(const LLSD& sd, const std::string& fallback)
{
    return sd.isString() ? sd.asString() : fallback;
}

template <> inline LLSD ss_atmoenv_value_to_sd<bool>(const bool& v) { return v; }
template <> inline bool ss_atmoenv_value_from_sd<bool>(const LLSD& sd, const bool& fallback)
{
    return sd.isBoolean() ? sd.asBoolean() : fallback;
}

template <> inline LLSD ss_atmoenv_value_to_sd<LLUUID>(const LLUUID& v) { return v; }
template <> inline LLUUID ss_atmoenv_value_from_sd<LLUUID>(const LLSD& sd, const LLUUID& fallback)
{
    return sd.isUUID() ? sd.asUUID() : fallback;
}

template <> inline LLSD ss_atmoenv_value_to_sd<LLColor3>(const LLColor3& v)
{
    LLSD sd = LLSD::emptyArray();
    sd.append((LLSD::Real)v.mV[0]);
    sd.append((LLSD::Real)v.mV[1]);
    sd.append((LLSD::Real)v.mV[2]);
    return sd;
}
template <> inline LLColor3 ss_atmoenv_value_from_sd<LLColor3>(const LLSD& sd, const LLColor3& fallback)
{
    if (!sd.isArray() || sd.size() < 3) return fallback;
    return LLColor3((F32)sd[0].asReal(), (F32)sd[1].asReal(), (F32)sd[2].asReal());
}

template <> inline LLSD ss_atmoenv_value_to_sd<LLVector2>(const LLVector2& v)
{
    LLSD sd = LLSD::emptyArray();
    sd.append((LLSD::Real)v.mV[0]);
    sd.append((LLSD::Real)v.mV[1]);
    return sd;
}
template <> inline LLVector2 ss_atmoenv_value_from_sd<LLVector2>(const LLSD& sd, const LLVector2& fallback)
{
    if (!sd.isArray() || sd.size() < 2) return fallback;
    return LLVector2((F32)sd[0].asReal(), (F32)sd[1].asReal());
}

template <typename T>
class SSAtmoEnvKeyframed
{
public:
    explicit SSAtmoEnvKeyframed(const T& default_value = T()) : mPlainValue(default_value) {}

    bool hasKeyframes() const { return !mKeyframes.empty(); }
    size_t keyframeCount() const { return mKeyframes.size(); }
    const std::vector<SSAtmoEnvKeyframe<T>>& keyframes() const { return mKeyframes; }
    std::vector<SSAtmoEnvKeyframe<T>>& keyframes() { return mKeyframes; }

    T valueAt(F64 phase) const
    {
        if (mKeyframes.empty()) return mPlainValue;
        if (mKeyframes.size() == 1) return mKeyframes.front().mValue;

        const SSAtmoEnvKeyframe<T>* a;
        const SSAtmoEnvKeyframe<T>* b;
        F32 t = 0.f;
        segmentAt(phase, a, b, t);
        return ss_atmoenv_lerp(a->mValue, b->mValue, t);
    }

    // <SS:Nexii> The crossfade valueAt() cannot express. Discrete values hold between keyframes (ss_atmoenv_lerp keeps the earlier one), so a field travelling between two textures CUTS when the day cycle plays through. blendAt() hands back the pair the phase sits between and the eased weight valueAt used to pick the survivor, so a moving cycle fades instead of snapping. False when there is nothing to fade - a plain or single-keyframe value, a HOLD, an equal pair, or a weight already at a rail.
    bool blendAt(F64 phase, T& out_from, T& out_to, F32& out_blend) const
    {
        if (mKeyframes.size() < 2) return false;

        const SSAtmoEnvKeyframe<T>* a;
        const SSAtmoEnvKeyframe<T>* b;
        F32 t = 0.f;
        segmentAt(phase, a, b, t);

        if (t <= 0.f || t >= 1.f || !(a->mValue != b->mValue)) return false;

        out_from = a->mValue;
        out_to = b->mValue;
        out_blend = t;
        return true;
    }

    static constexpr F64 PHASE_EPSILON = 0.001;

    bool hasKeyframeAt(F64 phase, F64 epsilon = PHASE_EPSILON) const
    {
        return findAt(wrapPhase(phase), epsilon) >= 0;
    }

    // <SS:Nexii> Follows a rename through the whole field - the plain value and every keyframe. Only meaningful for the string fields that name something by key (a precipitation type), and only instantiated where called, so the numeric fields never see it.
    void renameValue(const T& from, const T& to)
    {
        if (mPlainValue == from) mPlainValue = to;
        for (SSAtmoEnvKeyframe<T>& kf : mKeyframes)
        {
            if (kf.mValue == from) kf.mValue = to;
        }
    }

    void setValueAtHead(F64 head_phase, const T& value, F64 epsilon = PHASE_EPSILON)
    {
        if (mKeyframes.empty())
        {
            mPlainValue = value;
            return;
        }

        head_phase = wrapPhase(head_phase);

        const S32 at = findAt(head_phase, epsilon);
        if (at >= 0)
        {
            mKeyframes[at].mValue = value;
            return;
        }

        insertKeyframe(head_phase, value, ss_atmoenv_default_curve<T>());
    }

    void toggleKeyframeAtHead(F64 head_phase, F64 epsilon = PHASE_EPSILON)
    {
        head_phase = wrapPhase(head_phase);

        const S32 at = findAt(head_phase, epsilon);
        if (at >= 0)
        {
            mKeyframes.erase(mKeyframes.begin() + at);
            if (mKeyframes.empty())
            {
                mPlainValue = valueAt(head_phase);
            }
            return;
        }

        insertKeyframe(head_phase, valueAt(head_phase), ss_atmoenv_default_curve<T>());
    }

    // <SS:Nexii> Lays a keyframe down outright rather than through the editing head. Every other
    // way into this vector is head-relative (setValueAtHead, toggleKeyframeAtHead) because the
    // editor only ever authors at the preview position; a generator writes a whole curve at once
    // and has no head to speak of - see SSAtmoEnvWeatherGenerator. Times wrap and the insert keeps
    // the vector sorted, so keys may be handed over in any order.
    void addKeyframe(F64 time, const T& value, SSAtmoEnvCurve curve = ss_atmoenv_default_curve<T>())
    {
        insertKeyframe(wrapPhase(time), value, curve);
    }

    // Drops every keyframe and leaves the field the plain constant given.
    void reset(const T& plain)
    {
        mKeyframes.clear();
        mPlainValue = plain;
    }

    void collapseIfConstant(F32 epsilon)
    {
        if (mKeyframes.empty()) return;
        const T first = mKeyframes.front().mValue;
        for (size_t i = 1; i < mKeyframes.size(); ++i)
        {
            if (!ss_atmoenv_near_equal(mKeyframes[i].mValue, first, epsilon)) return;
        }
        mPlainValue = first;
        mKeyframes.clear();
    }

    void setCurveAt(F64 phase, SSAtmoEnvCurve curve, F64 epsilon = PHASE_EPSILON)
    {
        const S32 at = findAt(wrapPhase(phase), epsilon);
        if (at >= 0) mKeyframes[at].mCurve = curve;
    }

    F64 nextKeyframeTime(F64 head_phase) const
    {
        if (mKeyframes.empty()) return head_phase;
        head_phase = wrapPhase(head_phase);
        for (const SSAtmoEnvKeyframe<T>& kf : mKeyframes)
        {
            if (kf.mTime > head_phase + 1e-6) return kf.mTime;
        }
        return mKeyframes.front().mTime;
    }

    F64 prevKeyframeTime(F64 head_phase) const
    {
        if (mKeyframes.empty()) return head_phase;
        head_phase = wrapPhase(head_phase);
        for (auto it = mKeyframes.rbegin(); it != mKeyframes.rend(); ++it)
        {
            if (it->mTime < head_phase - 1e-6) return it->mTime;
        }
        return mKeyframes.back().mTime;
    }

    LLSD asLLSD() const
    {
        if (mKeyframes.empty())
        {
            return ss_atmoenv_value_to_sd(mPlainValue);
        }

        LLSD sd = LLSD::emptyMap();
        LLSD kfs = LLSD::emptyArray();
        for (const SSAtmoEnvKeyframe<T>& kf : mKeyframes)
        {
            LLSD entry = LLSD::emptyMap();
            entry["time"] = kf.mTime;
            entry["value"] = ss_atmoenv_value_to_sd(kf.mValue);
            entry["curve"] = ss_atmoenv_curve_name(kf.mCurve);
            kfs.append(entry);
        }
        sd["keyframes"] = kfs;
        return sd;
    }

    void fromLLSD(const LLSD& sd, const T& fallback)
    {
        mKeyframes.clear();

        if (sd.isMap() && sd.has("keyframes") && sd["keyframes"].isArray())
        {
            for (const LLSD& entry : llsd::inArray(sd["keyframes"]))
            {
                SSAtmoEnvKeyframe<T> kf;
                kf.mTime = wrapPhase(entry.has("time") ? entry["time"].asReal() : 0.0);
                kf.mValue = ss_atmoenv_value_from_sd<T>(entry["value"], fallback);
                kf.mCurve = ss_atmoenv_curve_on_load<T>(ss_atmoenv_curve_from_name(
                    entry.has("curve") ? entry["curve"].asString()
                                       : ss_atmoenv_curve_name(ss_atmoenv_default_curve<T>())));
                mKeyframes.push_back(kf);
            }
            std::sort(mKeyframes.begin(), mKeyframes.end(),
                      [](const SSAtmoEnvKeyframe<T>& a, const SSAtmoEnvKeyframe<T>& b) { return a.mTime < b.mTime; });
            mPlainValue = mKeyframes.empty() ? fallback : mKeyframes.front().mValue;
            return;
        }

        mPlainValue = ss_atmoenv_value_from_sd<T>(sd, fallback);
    }

private:
    static F64 wrapPhase(F64 phase)
    {
        phase = std::fmod(phase, 1.0);
        if (phase < 0.0) phase += 1.0;
        return phase;
    }

    // The keyframe pair phase sits between and the eased 0..1 weight through it. A HOLD curve weighs everything onto the earlier keyframe (t 0) - the old hold branches' answer - so valueAt is this plus one lerp. Always produces a pair for a multi-keyframe field: the wrap segment (last back to first) covers the phase outside [first, last], and the sorted keys guarantee a match inside it.
    void segmentAt(F64 phase, const SSAtmoEnvKeyframe<T>*& a, const SSAtmoEnvKeyframe<T>*& b, F32& t) const
    {
        phase = wrapPhase(phase);

        const SSAtmoEnvKeyframe<T>& first = mKeyframes.front();
        const SSAtmoEnvKeyframe<T>& last  = mKeyframes.back();

        if (phase < first.mTime || phase > last.mTime)
        {
            const F64 span = (first.mTime + 1.0) - last.mTime;
            const F64 elapsed = (phase < first.mTime) ? (phase + 1.0 - last.mTime)
                                                      : (phase - last.mTime);
            t = span > 0.0 ? (F32)(elapsed / span) : 0.f;
            if (last.mCurve == SSAtmoEnvCurve::EASE)
            {
                t = cubic_step(t);
            }
            else if (last.mCurve == SSAtmoEnvCurve::HOLD)
            {
                t = 0.f;
            }
            a = &last;
            b = &first;
            return;
        }

        for (size_t i = 0; i + 1 < mKeyframes.size(); ++i)
        {
            const SSAtmoEnvKeyframe<T>& ka = mKeyframes[i];
            const SSAtmoEnvKeyframe<T>& kb = mKeyframes[i + 1];
            if (phase < ka.mTime || phase > kb.mTime) continue;

            t = 0.f;
            if (ka.mCurve != SSAtmoEnvCurve::HOLD)
            {
                const F64 span = kb.mTime - ka.mTime;
                t = span > 0.0 ? (F32)((phase - ka.mTime) / span) : 0.f;
                if (ka.mCurve == SSAtmoEnvCurve::EASE)
                {
                    t = cubic_step(t);
                }
            }
            a = &ka;
            b = &kb;
            return;
        }

        a = &last;
        b = &last;
        t = 0.f;
    }

    S32 findAt(F64 time, F64 epsilon) const
    {
        for (size_t i = 0; i < mKeyframes.size(); ++i)
        {
            if (llabs(mKeyframes[i].mTime - time) < epsilon) return (S32)i;
        }
        return -1;
    }

    void insertKeyframe(F64 time, const T& value, SSAtmoEnvCurve curve)
    {
        SSAtmoEnvKeyframe<T> kf;
        kf.mTime = time;
        kf.mValue = value;
        kf.mCurve = curve;

        auto it = std::lower_bound(mKeyframes.begin(), mKeyframes.end(), time,
            [](const SSAtmoEnvKeyframe<T>& k, F64 t) { return k.mTime < t; });
        mKeyframes.insert(it, kf);
    }

    T mPlainValue{};
    std::vector<SSAtmoEnvKeyframe<T>> mKeyframes;
};

#endif
