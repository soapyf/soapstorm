// Viewport Avatar Toolset - batch retargeting and saved mappings (spec 07 RT-12..RT-14).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/batch_retarget.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

#include "vats/anim_convert.h"
#include "vats/fbx.h"
#include "vats/footlock.h"
#include "vats/json.h"
#include "vats/project.h"
#include "guard.h"

namespace vats {
namespace fs = std::filesystem;
namespace {

fs::path u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string str(const fs::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}
std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool read_bytes(const std::string& path, std::vector<std::uint8_t>& out) {
    std::ifstream f(u8(path), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !f.bad();
}

}  // namespace

std::string rig_table_json(const std::string& name, const BoneMap& map, const SourceAnim& src) {
    Json bones = Json::object();
    auto entry = [&](const std::string& sl) {
        Json names = Json::array();
        if (auto it = map.find(sl); it != map.end() && it->second >= 0 && it->second < int(src.joints.size()))
            names.push(src.joints[size_t(it->second)].name);
        bones.set(sl, std::move(names));
    };
    std::set<std::string> done;
    for (auto& [sl, j] : map) {
        if (done.count(sl)) continue;
        if (auto l = sl.find("Left"); l != std::string::npos) {  // the right side right after, overriding the copy
            std::string right = sl;
            right.replace(l, 4, "Right");
            entry(sl), entry(right);
            done.insert(sl), done.insert(right);
        } else if (auto r = sl.find("Right"); r != std::string::npos) {  // only the right side is mapped
            std::string left = sl;
            left.replace(r, 5, "Left");
            if (!done.count(left)) entry(left), entry(sl), done.insert(left), done.insert(sl);
        } else {
            entry(sl);
            done.insert(sl);
        }
    }
    Json t = Json::object();
    t.set("name", name);
    t.set("bones", std::move(bones));
    return write_json(t);
}

std::vector<RigTable> load_rig_tables(const std::string& dir) {
    std::vector<RigTable> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(u8(dir), ec)) {
        if (e.path().extension() != ".json") continue;
        std::vector<std::uint8_t> bytes;
        RigTable table;
        std::string err;
        if (read_bytes(str(e.path()), bytes) &&
            parse_rig_table(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), table, err))
            out.push_back(std::move(table));
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.name < b.name; });
    return out;
}

bool is_mixamo(const SourceAnim& src) {
    for (const SourceJoint& j : src.joints)
        if (lower(j.name).find("mixamorig") != std::string::npos) return true;
    return false;
}

bool read_source_file(const std::string& path, SourceAnim& out, std::string& err) {
    std::vector<std::uint8_t> bytes;
    if (!read_bytes(path, bytes) || bytes.empty()) return err = "could not be read", false;
    const std::string ext = lower(str(u8(path).extension()));
    return guarded(err, [&] {
        if (ext == ".bvh")
            return read_bvh_source(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), out, err);
        if (ext == ".fbx") return read_fbx_source(bytes, out, err);
        return read_gltf_source(bytes, str(u8(path).parent_path()), out, err);
    });
}

BatchReport batch_retarget(const Skeleton& skel, const Rig& rig, const std::string& folder,
                           const BatchRetargetOptions& opt, const BatchWrite& write) {
    BatchReport rep;
    const fs::path out_dir = u8(folder) / "retargeted";
    rep.out_dir = str(out_dir);
    std::vector<fs::path> files;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(u8(folder), ec)) {
        const std::string ext = lower(str(e.path().extension()));
        if (e.is_regular_file(ec) && (ext == ".bvh" || ext == ".fbx" || ext == ".gltf" || ext == ".glb"))
            files.push_back(e.path());
    }
    std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return lower(str(a.filename())) < lower(str(b.filename())); });

    std::set<std::string> used;  // output names written in this run, lowercase
    for (const fs::path& path : files) {
        BatchRow row;
        row.file = str(path.filename());
        SourceAnim src;
        std::string err;
        if (!read_source_file(str(path), src, err)) {
            row.notes = err.empty() ? "could not be read" : err;
            rep.rows.push_back(std::move(row));
            continue;
        }
        rep.mixamo = rep.mixamo || is_mixamo(src);
        BoneMap map;
        const int t = opt.table >= 0 && opt.table < int(opt.tables.size()) ? opt.table : best_rig_table(opt.tables, src, map);
        if (t >= 0 && t == opt.table) apply_rig_table(opt.tables[size_t(t)], src, map);
        std::vector<std::string> missing;
        if (t < 0 || !map_is_usable(map, &missing)) {
            row.notes = t < 0 ? "no rig table matches its bone names" : opt.tables[size_t(t)].name + " does not map ";
            for (size_t i = 0; t >= 0 && i < missing.size(); ++i) row.notes += (i ? ", " : "") + missing[i];
            rep.rows.push_back(std::move(row));
            continue;
        }

        RetargetResult r = retarget(skel, src, map, opt.retarget);
        if (opt.lock_feet) {
            FootLockOptions fl;
            fl.shape = opt.retarget.shape;
            lock_feet(r.clip, rig, fl);
        }
        const FitReport fit = fit_to_limits(skel, r.clip, opt.fit);
        row.fits = fit.fits;
        row.bytes = fit.bytes_after;
        row.frames = r.clip.end_frame + 1;
        row.fps = r.clip.fps;
        row.notes = opt.tables[size_t(t)].name + " rig";
        for (auto& s : fit.steps) row.notes += "; " + s;
        if (fit.too_long) row.notes += "; over 60 s: split or trim it in File > Import Animation (Retarget)...";

        std::string data;
        if (opt.anim) {
            AnimExportOptions eo;
            eo.shape = opt.fit.shape;
            if (const Json* red = r.clip.export_settings.find("reduce"); red && red->is_array() && red->arr.size() == 2)
                eo.reduce_rot_deg = red->arr[0].num, eo.reduce_pos_m = red->arr[1].num;
            const AnimExportResult ex = export_anim(skel, r.clip, eo);
            if (!ex.errors.empty()) {
                row.notes += "; not written: " + ex.errors[0];
                rep.rows.push_back(std::move(row));
                continue;
            }
            const std::vector<std::uint8_t> bytes = write_anim(ex.file);
            data.assign(bytes.begin(), bytes.end());
            row.bytes = bytes.size();
        } else {
            Project p;
            p.clip = std::move(r.clip);
            data = save_project(p);
        }
        const std::string ext = opt.anim ? ".anim" : ".vat";
        std::string name = str(path.stem()) + ext;
        if (used.count(lower(name))) name = str(path.stem()) + "_" + lower(str(path.extension())).substr(1) + ext;
        used.insert(lower(name));
        fs::create_directories(out_dir, ec);
        if (write(str(out_dir / u8(name)), data, err)) row.output = name;
        else row.notes += "; not written: " + err;
        rep.rows.push_back(std::move(row));
    }
    return rep;
}

}  // namespace vats
