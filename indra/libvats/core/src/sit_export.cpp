// Viewport Avatar Toolset - sit-system lines for couples and groups (AVsitter2 AVpos, nPose V4).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// The formats, as their scripts read and write them (both repositories checked 2026-09-27):
//
// AVsitter2 (github.com/AVsitter/AVsitter, MPL-2.0; docs avsitter.github.io/avsitter2_avpos.html):
//   SITTER <n>|<label>          n from 0, one section per [AV]sitA/B pair in the prim
//   SYNC <menu_name>|<anim>     a pose played by every SITTER that has a SYNC of the same menu name
//   {<menu_name>}<x,y,z><rx,ry,rz>
//   [AV]sitA.lsl reads the braces line as position and rotation vectors and seats with
//   PRIM_ROT_LOCAL llEuler2Rot(rot * DEG_TO_RAD) and PRIM_POS_LOCAL pos (in the root's frame when the scripts are
//   in the root). It cuts menu names to 23 characters (llGetSubString(part0, 0, 22)). [AV]adjuster.lsl's DUMP
//   writes the vectors with FormatFloat(pos, 3) and FormatFloat(rot, 1): rounded, trailing zeros and point
//   dropped, no spaces.
//
// nPose V4 (github.com/nPoseTeam/nPose-V4, GPLv2 with a full-perms addendum; wiki page NC-Contents, XANIM):
//   XANIM|<seat#>|<anim>|<x, y, z>|<rx, ry, rz>
//   seats from 1, created by SEAT_INIT|<count> in the .init card (V4 removed V3's ANIM line). nPose Core reads
//   the rotation as llEuler2Rot(rot * DEG_TO_RAD); the position is from the root prim (or the slave script's prim,
//   see adjustRefRoot). nPose Slave's dump writes vectorToString(pos, 3) and vectorToString(rot, 2): rounded,
//   trailing zeros and point dropped, ", " between components.
#include "vats/sit_export.h"

#include <cstdio>

namespace vats {
namespace {

// Rounded to `decimals`, without trailing zeros or point, as both dumps write numbers; never "-0".
std::string num(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    return s == "-0" ? "0" : s;
}

std::string vec(const Vec3& v, int decimals, const char* sep) {
    return "<" + num(v.x, decimals) + sep + num(v.y, decimals) + sep + num(v.z, decimals) + ">";
}

// The first n characters of UTF-8 text (LSL counts characters, not bytes).
std::string first_chars(const std::string& s, int n) {
    size_t i = 0;
    for (int k = 0; i < s.size(); ++i)
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80 && k++ == n) break;
    return s.substr(0, i);
}

}  // namespace

void sitter_in_root(const Sitter& s, const SitRoot& root, Vec3& pos, Vec3& euler_deg) {
    const Xform x = root.xform() * s.place;
    pos = x.pos;
    euler_deg = quat_to_euler(x.rot);  // the Euler order llRot2Euler returns, y in [-90, 90]
}

std::string avsitter_lines(const std::string& pose, const std::vector<Sitter>& sitters, const SitRoot& root) {
    const std::string name = first_chars(pose, 23);
    std::string out;
    for (size_t i = 0; i < sitters.size(); ++i) {
        Vec3 pos, rot;
        sitter_in_root(sitters[i], root, pos, rot);
        if (i) out += "\n";
        out += "SITTER " + std::to_string(i) + "|" + sitters[i].name + "\n";
        out += "SYNC " + name + "|" + sitters[i].anim + "\n";
        out += "{" + name + "}" + vec(pos, 3, ",") + vec(rot, 1, ",") + "\n";
    }
    return out;
}

std::string npose_lines(const std::vector<Sitter>& sitters, const SitRoot& root) {
    std::string out;
    for (size_t i = 0; i < sitters.size(); ++i) {
        Vec3 pos, rot;
        sitter_in_root(sitters[i], root, pos, rot);
        out += "XANIM|" + std::to_string(i + 1) + "|" + sitters[i].anim + "|" + vec(pos, 3, ", ") + "|" +
               vec(rot, 2, ", ") + "\n";
    }
    return out;
}

}  // namespace vats
