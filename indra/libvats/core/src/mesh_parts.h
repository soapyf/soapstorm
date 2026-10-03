// Viewport Avatar Toolset - how the mesh readers put their parts, groups and shape keys into a DaeModel (core-private).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "vats/dae.h"

namespace vats {

// A shape key's offsets on the vertices of one build (a part's triangles of one material), as a reader finds them.
struct KeyBuild {
    std::vector<std::uint32_t> vertices;  // the build's vertices, ascending
    std::vector<float> dpos, dnrm;        // 3 per vertex; dnrm empty when the file gives no normal offsets
    double initial = 0;

    // A vertex that does not move is left out, so a key on a face costs nothing on the rest of the body.
    void add(std::uint32_t v, const Vec3& dp, const Vec3* dn) {
        if (dp.length() < 1e-9 && (!dn || dn->length() < 1e-7)) return;
        if (dn && dnrm.size() < dpos.size()) dnrm.resize(dpos.size(), 0.f);  // normals for every vertex or for none
        vertices.push_back(v);
        for (int i = 0; i < 3; ++i) dpos.push_back(float(dp[i]));
        if (dn || !dnrm.empty())
            for (int i = 0; i < 3; ++i) dnrm.push_back(dn ? float((*dn)[i]) : 0.f);
    }
};

// Opens the next part: the groups appended after it are its own.
inline void begin_part(DaeModel& m, const std::string& name) {
    m.parts.push_back({name, std::uint32_t(m.vertex_count()), 0, std::uint32_t(m.indices.size()), 0});
}

// Appends a build to the open part as a group of its material. B has pos, nrm, uv, joints, weights (per vertex) and
// idx (triangles, local to the build). keys: the build's shape key offsets by name, merged into the part's entries.
template <class B>
void append_group(DaeModel& m, int material, const B& b, const std::map<std::string, KeyBuild>& keys = {}) {
    if (m.parts.empty()) begin_part(m, "");
    DaePart& part = m.parts.back();
    const DaeGroup g{material, std::uint32_t(m.vertex_count()), std::uint32_t(b.pos.size() / 3), std::uint32_t(m.indices.size()),
                     std::uint32_t(b.idx.size())};
    for (std::uint32_t i : b.idx) m.indices.push_back(g.first_vertex + i);
    m.positions.insert(m.positions.end(), b.pos.begin(), b.pos.end());
    m.normals.insert(m.normals.end(), b.nrm.begin(), b.nrm.end());
    m.uvs.insert(m.uvs.end(), b.uv.begin(), b.uv.end());
    m.joints.insert(m.joints.end(), b.joints.begin(), b.joints.end());
    m.weights.insert(m.weights.end(), b.weights.begin(), b.weights.end());
    m.groups.push_back(g);
    part.vertex_count += g.vertex_count;
    part.index_count += g.index_count;
    const size_t first_key = [&] {  // the open part's entries are the last ones: their vertices start in it
        size_t k = m.shape_keys.size();
        while (k > 0 && !m.shape_keys[k - 1].vertices.empty() && m.shape_keys[k - 1].vertices.front() >= part.first_vertex) --k;
        return k;
    }();
    for (const auto& [name, kb] : keys) {
        if (kb.vertices.empty()) continue;
        DaeShapeKey* e = nullptr;
        for (size_t k = first_key; k < m.shape_keys.size() && !e; ++k)
            if (m.shape_keys[k].name == name) e = &m.shape_keys[k];
        if (!e) e = &m.shape_keys.emplace_back(), e->name = name, e->initial = kb.initial;
        // Normal offsets for every vertex or for none: zeros fill in when one material's build has them and another not.
        if (!kb.dnrm.empty() && e->dnrm.empty()) e->dnrm.assign(e->dpos.size(), 0.f);
        for (std::uint32_t v : kb.vertices) e->vertices.push_back(g.first_vertex + v);
        e->dpos.insert(e->dpos.end(), kb.dpos.begin(), kb.dpos.end());
        if (!kb.dnrm.empty()) e->dnrm.insert(e->dnrm.end(), kb.dnrm.begin(), kb.dnrm.end());
        else if (!e->dnrm.empty()) e->dnrm.resize(e->dpos.size(), 0.f);
    }
}

}  // namespace vats
