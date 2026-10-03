// Viewport Avatar Toolset - soft-body volumes.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/soft_body.h"

#include <algorithm>
#include <cmath>

namespace vats {

const std::vector<SoftBodyPart>& soft_body_parts() {
    // The volumes RM-9 puts soft-body helpers on (CHEST for a breast bone in the middle), and the upper legs a butt
    // cheek shares its weight with.
    static const std::vector<SoftBodyPart> parts = {
        {"BUTT", "Butt"},
        {"LEFT_PEC", "Left Breast"},
        {"RIGHT_PEC", "Right Breast"},
        {"CHEST", "Chest"},
        {"BELLY", "Belly"},
        {"LEFT_HANDLE", "Left Love Handle"},
        {"RIGHT_HANDLE", "Right Love Handle"},
        {"UPPER_BACK", "Upper Back"},
        {"LOWER_BACK", "Lower Back"},
        {"L_UPPER_LEG", "Left Upper Leg"},
        {"R_UPPER_LEG", "Right Upper Leg"}};
    return parts;
}

const char* soft_body_name(std::string_view volume) {
    for (const SoftBodyPart& p : soft_body_parts())
        if (volume == p.volume) return p.name;
    return nullptr;
}

namespace {

// The joint an SK-40 index moves with: itself, or the joint a volume hangs from; -1 for mRoot.
int owner(const Skeleton& skel, int j) {
    const int root = dae_root(skel);
    if (j < root) return j;
    if (j > root && j - root - 1 < static_cast<int>(skel.volumes().size())) return skel.volumes()[size_t(j - root - 1)].joint;
    return -1;
}

}  // namespace

std::vector<int> soft_body_partner(const Skeleton& skel, const DaeModel& model, int volume) {
    const int root = dae_root(skel);
    if (volume <= root || volume - root - 1 >= static_cast<int>(skel.volumes().size())) return {};
    const int node = skel.volumes()[size_t(volume - root - 1)].node;
    const bool middle = skel.mirror(node) == node;  // BUTT, BELLY: the two sides are one partner
    auto group = [&](int j) {
        const int o = owner(skel, j);
        return o < 0 ? -1 : middle ? std::min(o, skel.mirror(o)) : o;
    };
    std::vector<double> shared(size_t(skel.size()), 0);
    const size_t nv = std::min(model.joints.size(), model.weights.size()) / 4;
    for (size_t v = 0; v < nv; ++v) {
        bool on = false;
        for (int k = 0; k < 4; ++k) on = on || (model.joints[v * 4 + k] == volume && model.weights[v * 4 + k] > 0);
        if (!on) continue;
        for (int k = 0; k < 4; ++k)
            if (const int j = model.joints[v * 4 + k]; j != volume && model.weights[v * 4 + k] > 0)
                if (const int g = group(j); g >= 0) shared[size_t(g)] += model.weights[v * 4 + k];
    }
    const auto best = std::max_element(shared.begin(), shared.end());
    if (*best <= 0) return {};
    int g = int(best - shared.begin());
    // Flesh shared with the volume's own joint moves with it anyway unless the volume is keyed or bounced; another
    // joint sharing at least half as much (a cheek's thigh) is the share that shows when the body moves.
    const int own = group(volume);
    for (int k = 0; k < skel.size(); ++k)
        if (k != own && shared[size_t(k)] >= 0.5 * *best && (g == own || shared[size_t(k)] > shared[size_t(g)])) g = k;
    std::vector<int> out;
    for (int j = 0; j < dae_index_count(skel); ++j)
        if (j != volume && j != root && group(j) == g) out.push_back(j);
    return out;
}

std::string soft_body_partner_name(const Skeleton& skel, const std::vector<int>& partner) {
    std::vector<std::string> joints;
    for (int j : partner)
        if (const int o = owner(skel, j); o >= 0 && std::find(joints.begin(), joints.end(), skel[o].name) == joints.end())
            joints.push_back(skel[o].name);
    std::sort(joints.begin(), joints.end());  // Left before Right
    std::string out;
    for (size_t i = 0; i < joints.size(); ++i) out += (i == 0 ? "" : i + 1 == joints.size() ? " and " : ", ") + joints[i];
    return out;
}

void share_soft_body(const Skeleton& skel, DaeModel& model, int volume, double share) {
    const std::vector<int> partner = soft_body_partner(skel, model, volume);
    if (partner.empty()) return;
    share = std::clamp(share, 0.0, 1.0);
    const size_t nv = std::min(model.joints.size(), model.weights.size()) / 4;
    for (size_t v = 0; v < nv; ++v) {
        int* j = &model.joints[v * 4];
        float* w = &model.weights[v * 4];
        double own = 0, other = 0;
        for (int k = 0; k < 4; ++k) {
            if (!(w[k] > 0)) continue;
            if (j[k] == volume) own += w[k];
            else if (std::find(partner.begin(), partner.end(), j[k]) != partner.end()) other += w[k];
        }
        if (own <= 0 || other <= 0) continue;  // flesh on one of them alone is no shared flesh
        const double total = own + other, r = own / total;
        const double now = share <= 0.5 ? r * share * 2 : r + (1 - r) * (share - 0.5) * 2;
        const double k_own = total * now / own, k_other = total * (1 - now) / other;
        for (int k = 0; k < 4; ++k) {
            if (!(w[k] > 0)) continue;
            if (j[k] == volume) w[k] = float(w[k] * k_own);
            else if (std::find(partner.begin(), partner.end(), j[k]) != partner.end()) w[k] = float(w[k] * k_other);
        }
        for (int a = 1; a < 4; ++a)  // largest first, as the readers store them
            for (int b = a; b > 0 && w[b] > w[b - 1]; --b) std::swap(w[b], w[b - 1]), std::swap(j[b], j[b - 1]);
    }
}

}  // namespace vats
