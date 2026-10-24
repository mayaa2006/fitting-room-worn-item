#include "NifExport.h"

#include "ArmorExportModel.h"

#include <NifFile.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <memory>
#include <set>
#include <sstream>

// ⚠ NO PCH.h HERE ON PURPOSE. This file speaks nifly, whose NiNode/NiShape
// live in namespace nifly, and it must stay engine-free so the same source
// builds into NifExportTests on any platform. CMake excludes it from the
// plugin's precompiled header.

namespace OS::NifExport {

    namespace {
        using namespace nifly;

        bool LoadBytes(NifFile& a_nif, const std::vector<std::uint8_t>& a_bytes, std::string& a_err) {
            if (a_bytes.empty()) {
                a_err = "empty file";
                return false;
            }
            std::string        buf(reinterpret_cast<const char*>(a_bytes.data()), a_bytes.size());
            std::istringstream in(buf, std::ios::in | std::ios::binary);
            // ⚠ nifly THROWS on some malformed input (std::length_error
            // "Block index is too high", found by fuzzing) instead of returning
            // an error. One broken mesh in someone's mod must cost one piece,
            // not the whole export, so every entry point below also catches.
            try {
                if (a_nif.Load(in) != 0 || !a_nif.IsValid()) {
                    a_err = "not a NIF nifly can read";
                    return false;
                }
            } catch (const std::exception& e) {
                a_err = std::string("unreadable NIF (") + e.what() + ")";
                return false;
            }
            return true;
        }

        bool SaveBytes(NifFile& a_nif, std::vector<std::uint8_t>& a_out, std::string& a_err) {
            std::ostringstream out(std::ios::out | std::ios::binary);
            if (a_nif.Save(out) != 0) {
                a_err = "nifly could not write the NIF";
                return false;
            }
            const auto s = out.str();
            a_out.assign(s.begin(), s.end());
            return !a_out.empty();
        }

        void Collect(NifFile& a_nif, NiNode* a_node, std::vector<NiShape*>& a_out,
                     std::set<NiNode*>& a_seen) {
            if (!a_node || !a_seen.insert(a_node).second) {
                return;  // a cycle in a broken file must not hang the export
            }
            for (auto& ref : a_node->childRefs) {
                auto* obj = a_nif.GetHeader().GetBlock<NiAVObject>(ref);
                if (!obj) {
                    continue;
                }
                if (auto* shape = dynamic_cast<NiShape*>(obj)) {
                    a_out.push_back(shape);
                } else if (auto* node = dynamic_cast<NiNode*>(obj)) {
                    Collect(a_nif, node, a_out, a_seen);
                }
            }
        }

        std::vector<NiShape*> ShapesInOrder(NifFile& a_nif) {
            std::vector<NiShape*> out;
            std::set<NiNode*>     seen;
            Collect(a_nif, a_nif.GetRootNode(), out, seen);
            return out;
        }

        // The shape's own shader, cloned first if another shape shares it.
        NiShader* OwnShader(NifFile& a_nif, NiShape* a_shape) {
            auto* ref = a_shape->ShaderPropertyRef();
            if (!ref || ref->IsEmpty()) {
                return nullptr;
            }
            auto& hdr    = a_nif.GetHeader();
            auto* shader = hdr.GetBlock<NiShader>(*ref);
            if (!shader) {
                return nullptr;
            }
            if (hdr.GetBlockRefCount(ref->index) > 1) {
                auto clone = static_cast<const NiObject*>(shader)->Clone();
                ref->index = hdr.AddBlock(std::move(clone));
                shader     = hdr.GetBlock<NiShader>(*ref);
            }
            return shader;
        }

        // The shader's own texture set, cloned first if shared.
        BSShaderTextureSet* OwnTextureSet(NifFile& a_nif, NiShader* a_shader) {
            if (!a_shader || !a_shader->HasTextureSet()) {
                return nullptr;
            }
            auto& hdr = a_nif.GetHeader();
            auto* ref = a_shader->TextureSetRef();
            auto* ts  = hdr.GetBlock<BSShaderTextureSet>(*ref);
            if (!ts) {
                return nullptr;
            }
            if (hdr.GetBlockRefCount(ref->index) > 1) {
                auto clone = static_cast<const NiObject*>(ts)->Clone();
                ref->index = hdr.AddBlock(std::move(clone));
                ts         = hdr.GetBlock<BSShaderTextureSet>(*ref);
            }
            return ts;
        }

        void SetSlot(BSShaderTextureSet* a_ts, std::uint32_t a_slot, const std::string& a_path) {
            if (!a_ts) {
                return;
            }
            if (a_slot >= a_ts->textures.size()) {
                if (a_slot >= 9) {
                    return;  // SSE texture sets are 9 wide; nothing writes past
                }
                a_ts->textures.resize(a_slot + 1);
            }
            a_ts->textures[a_slot].get() = a_path;
        }
    }

    Result RewriteImpl(const std::vector<std::uint8_t>&          a_nif,
                   const std::vector<ShapeEdit>&             a_edits,
                   const std::map<std::string, std::string>& a_remap) {
        Result  r;
        NifFile nif;
        if (!LoadBytes(nif, a_nif, r.error)) {
            return r;
        }
        auto shapes = ShapesInOrder(nif);
        r.shapes    = static_cast<int>(shapes.size());

        std::map<std::string, int> fileNames;
        for (auto* s : shapes) {
            ++fileNames[s->name.get()];
        }
        std::map<std::string, int> editNames;
        for (const auto& e : a_edits) {
            ++editNames[e.name];
        }

        std::vector<NiShape*> toRemove;
        for (const auto& e : a_edits) {
            NiShape* target = nullptr;
            if (!e.name.empty() && editNames[e.name] == 1 && fileNames[e.name] == 1) {
                for (auto* s : shapes) {
                    if (s->name.get() == e.name) {
                        target = s;
                        break;
                    }
                }
            }
            // ⚠ POSITION ONLY WHEN THE NAME CANNOT DECIDE (blank, or not
            // unique). A name the file simply does not have is geometry the
            // engine added at runtime, and matching it by position would put
            // its textures on a real shape.
            const bool nameUndecidable =
                e.name.empty() || editNames[e.name] > 1 || fileNames[e.name] > 1;
            if (!target && nameUndecidable && e.order >= 0 &&
                e.order < static_cast<int>(shapes.size())) {
                target = shapes[static_cast<std::size_t>(e.order)];
            }
            if (!target) {
                ++r.unmatched;
                continue;
            }
            if (e.remove) {
                // Deleted after the remap below, which still walks `shapes`.
                if (std::find(toRemove.begin(), toRemove.end(), target) == toRemove.end()) {
                    toRemove.push_back(target);
                }
                ++r.edited;
                continue;
            }
            const bool touchesTextures = !e.textures.empty();
            if (!touchesTextures && !e.hasSpecular) {
                continue;
            }
            auto* shader = OwnShader(nif, target);
            if (!shader) {
                ++r.unmatched;
                continue;
            }
            if (touchesTextures) {
                auto* ts = OwnTextureSet(nif, shader);
                for (const auto& [slot, path] : e.textures) {
                    SetSlot(ts, slot, path);
                }
            }
            if (e.hasSpecular) {
                if (auto* ls = dynamic_cast<BSLightingShaderProperty*>(shader)) {
                    ls->specularColor    = Vector3(e.specular[0], e.specular[1], e.specular[2]);
                    ls->glossiness       = e.glossiness;
                    ls->specularStrength = e.specularStrength;
                }
            }
            ++r.edited;
        }

        if (!a_remap.empty()) {
            for (auto* s : shapes) {
                auto* shaderRef = s->ShaderPropertyRef();
                if (!shaderRef || shaderRef->IsEmpty()) {
                    continue;
                }
                auto* shader = nif.GetHeader().GetBlock<NiShader>(*shaderRef);
                if (!shader || !shader->HasTextureSet()) {
                    continue;
                }
                auto* ts = nif.GetHeader().GetBlock<BSShaderTextureSet>(*shader->TextureSetRef());
                if (!ts) {
                    continue;
                }
                // Changing a shared set is right here: every shape using that
                // texture gets the baked one, which is what a remap means.
                for (auto& t : ts->textures) {
                    const auto key = ArmorExportModel::TextureKey(t.get());
                    if (key.empty()) {
                        continue;
                    }
                    if (const auto it = a_remap.find(key); it != a_remap.end()) {
                        t.get() = it->second;
                        ++r.remapped;
                    }
                }
            }
        }

        for (auto* shape : toRemove) {
            nif.DeleteShape(shape);
            ++r.removed;
        }

        r.ok = SaveBytes(nif, r.bytes, r.error);
        return r;
    }

    Result MergeImpl(const std::vector<std::vector<std::uint8_t>>& a_nifs) {
        Result r;
        if (a_nifs.empty()) {
            r.error = "nothing to merge";
            return r;
        }
        if (a_nifs.size() == 1) {
            r.ok    = true;
            r.bytes = a_nifs.front();
            NifFile probe;
            std::string err;
            r.names.emplace_back();
            if (LoadBytes(probe, r.bytes, err)) {
                const auto shapes = ShapesInOrder(probe);
                r.shapes          = static_cast<int>(shapes.size());
                for (auto* s : shapes) {
                    r.names.back().emplace(s->name.get(), s->name.get());
                }
            }
            return r;
        }
        NifFile base;
        if (!LoadBytes(base, a_nifs.front(), r.error)) {
            r.error = "first model: " + r.error;
            return r;
        }
        std::set<std::string> taken;
        r.names.emplace_back();
        for (auto* s : ShapesInOrder(base)) {
            taken.insert(s->name.get());
            r.names.back().emplace(s->name.get(), s->name.get());
        }
        for (std::size_t i = 1; i < a_nifs.size(); ++i) {
            NifFile     src;
            std::string err;
            if (!LoadBytes(src, a_nifs[i], err)) {
                r.error = "model " + std::to_string(i + 1) + ": " + err;
                return r;
            }
            // Root extra data (a BODYTRI morph link, an SMP physics path) is
            // per FILE and only the base's survives: say so, do not hide it.
            // Only the links that matter count: BODYTRI (body sliders, which
            // the caller can rebuild from Result::names) and HDT/SMP physics.
            // BSXFlags and the like are on nearly every root.
            if (auto* root = src.GetRootNode()) {
                bool tri = false, physics = false;
                for (auto& ref : root->extraDataRefs) {
                    auto* xd = src.GetHeader().GetBlock<NiExtraData>(ref);
                    if (!xd) {
                        continue;
                    }
                    const std::string n = xd->name.get();
                    tri     = tri || n == "BODYTRI";
                    physics = physics || n.rfind("HDT", 0) == 0;
                }
                r.droppedTri += tri ? 1 : 0;
                r.droppedPhysics += physics ? 1 : 0;
            }
            r.names.emplace_back();
            for (auto* s : ShapesInOrder(src)) {
                const std::string original = s->name.get();
                std::string name = original;
                if (name.empty()) {
                    name = "Shape";
                }
                if (taken.count(name)) {
                    const std::string stem = name;
                    for (int n = 2;; ++n) {
                        name = stem + "_" + std::to_string(n);
                        if (!taken.count(name)) {
                            break;
                        }
                    }
                }
                if (!base.CloneShape(s, name, &src)) {
                    r.error = "could not copy shape '" + name + "'";
                    return r;
                }
                taken.insert(name);
                r.names.back().emplace(original, name);
            }
        }
        r.shapes = static_cast<int>(ShapesInOrder(base).size());
        r.ok     = SaveBytes(base, r.bytes, r.error);
        return r;
    }

    namespace {
        bool IsSkinShader(NiShader* a_shader) {
            auto* ls = dynamic_cast<BSLightingShaderProperty*>(a_shader);
            if (!ls) {
                return false;
            }
            const auto t = ls->GetShaderType();
            return t == BSLSP_FACE || t == BSLSP_SKINTINT || t == BSLSP_HAIRTINT;
        }

        // Calls a_fn on every texture path string of every armour shape.
        // a_own clones a shared texture set first (only when writing).
        template <class Fn>
        void ForEachArmourTexture(NifFile& a_nif, bool a_own, Fn&& a_fn) {
            for (auto* shape : ShapesInOrder(a_nif)) {
                auto* ref = shape->ShaderPropertyRef();
                if (!ref || ref->IsEmpty()) {
                    continue;
                }
                auto* shader = a_nif.GetHeader().GetBlock<NiShader>(*ref);
                if (!shader || IsSkinShader(shader)) {
                    continue;
                }
                if (auto* fx = dynamic_cast<BSEffectShaderProperty*>(shader)) {
                    a_fn(fx->sourceTexture.get());
                    a_fn(fx->greyscaleTexture.get());
                    a_fn(fx->envMapTexture.get());
                    a_fn(fx->normalTexture.get());
                    a_fn(fx->envMaskTexture.get());
                    continue;
                }
                if (!shader->HasTextureSet()) {
                    continue;
                }
                BSShaderTextureSet* ts =
                    a_own ? OwnTextureSet(a_nif, shader)
                          : a_nif.GetHeader().GetBlock<BSShaderTextureSet>(*shader->TextureSetRef());
                if (!ts) {
                    continue;
                }
                for (auto& t : ts->textures) {
                    a_fn(t.get());
                }
            }
        }
    }

    std::vector<std::string> LocalizableTexturesImpl(const std::vector<std::uint8_t>& a_nif) {
        NifFile                  nif;
        std::string              err;
        std::vector<std::string> out;
        if (!LoadBytes(nif, a_nif, err)) {
            return out;
        }
        std::set<std::string> seen;
        ForEachArmourTexture(nif, false, [&](std::string& a_path) {
            const auto key = ArmorExportModel::TextureKey(a_path);
            if (!key.empty() && seen.insert(key).second) {
                out.push_back(a_path);
            }
        });
        return out;
    }

    Result LocalizeImpl(const std::vector<std::uint8_t>& a_nif, const std::map<std::string, std::string>& a_map) {
        Result  r;
        NifFile nif;
        if (!LoadBytes(nif, a_nif, r.error)) {
            return r;
        }
        r.shapes = static_cast<int>(ShapesInOrder(nif).size());
        ForEachArmourTexture(nif, true, [&](std::string& a_path) {
            const auto key = ArmorExportModel::TextureKey(a_path);
            if (key.empty()) {
                return;
            }
            if (const auto it = a_map.find(key); it != a_map.end() && it->second != a_path) {
                a_path = it->second;
                ++r.remapped;
            }
        });
        r.ok = SaveBytes(nif, r.bytes, r.error);
        return r;
    }

    std::vector<std::string> ShapeNamesImpl(const std::vector<std::uint8_t>& a_nif) {
        NifFile     nif;
        std::string err;
        std::vector<std::string> out;
        if (!LoadBytes(nif, a_nif, err)) {
            return out;
        }
        for (auto* s : ShapesInOrder(nif)) {
            out.push_back(s->name.get());
        }
        return out;
    }

    std::vector<std::string> TexturePathsImpl(const std::vector<std::uint8_t>& a_nif, int a_order) {
        NifFile     nif;
        std::string err;
        std::vector<std::string> out;
        if (!LoadBytes(nif, a_nif, err)) {
            return out;
        }
        const auto shapes = ShapesInOrder(nif);
        if (a_order < 0 || a_order >= static_cast<int>(shapes.size())) {
            return out;
        }
        auto* shader = nif.GetShader(shapes[static_cast<std::size_t>(a_order)]);
        if (!shader || !shader->HasTextureSet()) {
            return out;
        }
        if (auto* ts = nif.GetHeader().GetBlock<BSShaderTextureSet>(*shader->TextureSetRef())) {
            for (const auto& t : ts->textures) {
                out.push_back(t.get());
            }
        }
        return out;
    }

    namespace {
        std::string Lower(std::string a_s) {
            for (auto& c : a_s) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return a_s;
        }

        bool IsBodyTri(NifFile& a_nif, const NiBlockRef<NiExtraData>& a_ref, std::string* a_value) {
            auto* sd = a_nif.GetHeader().GetBlock<NiStringExtraData>(a_ref);
            if (!sd || sd->name.get() != "BODYTRI") {
                return false;
            }
            if (a_value) {
                *a_value = sd->stringData.get();
            }
            return true;
        }

        // The root, then every shape: where a BODYTRI link can sit.
        std::vector<NiAVObject*> TriHolders(NifFile& a_nif) {
            std::vector<NiAVObject*> out;
            if (auto* root = a_nif.GetRootNode()) {
                out.push_back(root);
            }
            for (auto* s : ShapesInOrder(a_nif)) {
                out.push_back(s);
            }
            return out;
        }
    }

    std::string BodyTriPathImpl(const std::vector<std::uint8_t>& a_nif) {
        NifFile     nif;
        std::string err;
        if (!LoadBytes(nif, a_nif, err)) {
            return {};
        }
        for (auto* obj : TriHolders(nif)) {
            for (auto& ref : obj->extraDataRefs) {
                std::string v;
                if (IsBodyTri(nif, ref, &v) && !v.empty()) {
                    return v;
                }
            }
        }
        return {};
    }

    Result ApplyMorphsImpl(const std::vector<std::uint8_t>& a_nif, const TriFile::File& a_tri,
                           const std::map<std::string, float>& a_values) {
        Result  r;
        NifFile nif;
        if (!LoadBytes(nif, a_nif, r.error)) {
            return r;
        }
        const auto shapes = ShapesInOrder(nif);
        r.shapes          = static_cast<int>(shapes.size());
        for (const auto& ts : a_tri) {
            NiShape* target = nullptr;
            for (auto* s : shapes) {
                if (s->name.get() == ts.name) {
                    target = s;
                    break;
                }
            }
            if (!target) {
                const auto want = Lower(ts.name);
                int        hits = 0;
                for (auto* s : shapes) {
                    if (Lower(s->name.get()) == want) {
                        target = s;
                        ++hits;
                    }
                }
                if (hits != 1) {
                    target = nullptr;
                }
            }
            if (!target) {
                ++r.unmatched;
                continue;
            }
            std::vector<Vector3> verts;
            if (!nif.GetVertsForShape(target, verts) || verts.empty()) {
                continue;
            }
            bool moved = false;
            for (const auto& m : ts.morphs) {
                const auto it = a_values.find(m.name);
                if (it == a_values.end() || it->second == 0.0f || !std::isfinite(it->second)) {
                    continue;
                }
                const float v = it->second;
                for (const auto& d : m.deltas) {
                    if (d.index >= verts.size()) {
                        continue;
                    }
                    const auto off = m.Offset(d);
                    auto&      p   = verts[d.index];
                    p.x += off[0] * v;
                    p.y += off[1] * v;
                    p.z += off[2] * v;
                    moved = true;
                }
            }
            if (moved) {
                nif.SetVertsForShape(target, verts);
                ++r.edited;
            }
        }
        r.ok = SaveBytes(nif, r.bytes, r.error);
        return r;
    }

    Result SetBodyTriImpl(const std::vector<std::uint8_t>& a_nif, const std::string& a_path) {
        Result  r;
        NifFile nif;
        if (!LoadBytes(nif, a_nif, r.error)) {
            return r;
        }
        r.shapes = static_cast<int>(ShapesInOrder(nif).size());
        for (auto* obj : TriHolders(nif)) {
            std::vector<std::uint32_t> at;
            std::uint32_t              pos = 0;
            for (auto& ref : obj->extraDataRefs) {
                if (IsBodyTri(nif, ref, nullptr)) {
                    at.push_back(pos);
                }
                ++pos;
            }
            // Back to front, so earlier positions stay valid. The unlinked
            // block itself goes when the file is saved.
            for (auto it = at.rbegin(); it != at.rend(); ++it) {
                obj->extraDataRefs.RemoveBlockRef(*it);
            }
            if (!at.empty()) {
                ++r.remapped;
            }
        }
        if (!a_path.empty()) {
            auto* root = nif.GetRootNode();
            if (!root) {
                r.error = "the file has no root node";
                return r;
            }
            auto sd = std::make_unique<NiStringExtraData>();
            sd->name.get()       = "BODYTRI";
            sd->stringData.get() = a_path;
            nif.AssignExtraData(root, std::move(sd));
            ++r.edited;
        }
        r.ok = SaveBytes(nif, r.bytes, r.error);
        return r;
    }

    // ---- public entry points: nothing nifly throws gets past these ----------

    namespace {
        template <class Fn>
        Result Guard(Fn&& a_fn) {
            try {
                return a_fn();
            } catch (const std::exception& e) {
                Result r;
                r.error = std::string("NIF error: ") + e.what();
                return r;
            } catch (...) {
                Result r;
                r.error = "NIF error";
                return r;
            }
        }
        template <class Fn>
        std::vector<std::string> GuardList(Fn&& a_fn) {
            try {
                return a_fn();
            } catch (...) {
                return {};
            }
        }
    }

    Result Rewrite(const std::vector<std::uint8_t>& a_nif, const std::vector<ShapeEdit>& a_edits,
                   const std::map<std::string, std::string>& a_remap) {
        return Guard([&] { return RewriteImpl(a_nif, a_edits, a_remap); });
    }
    Result Merge(const std::vector<std::vector<std::uint8_t>>& a_nifs) {
        return Guard([&] { return MergeImpl(a_nifs); });
    }
    Result Localize(const std::vector<std::uint8_t>& a_nif, const std::map<std::string, std::string>& a_map) {
        return Guard([&] { return LocalizeImpl(a_nif, a_map); });
    }
    std::string BodyTriPath(const std::vector<std::uint8_t>& a_nif) {
        try {
            return BodyTriPathImpl(a_nif);
        } catch (...) {
            return {};
        }
    }
    Result ApplyMorphs(const std::vector<std::uint8_t>& a_nif, const TriFile::File& a_tri,
                       const std::map<std::string, float>& a_values) {
        return Guard([&] { return ApplyMorphsImpl(a_nif, a_tri, a_values); });
    }
    Result SetBodyTri(const std::vector<std::uint8_t>& a_nif, const std::string& a_path) {
        return Guard([&] { return SetBodyTriImpl(a_nif, a_path); });
    }
    std::vector<std::string> LocalizableTextures(const std::vector<std::uint8_t>& a_nif) {
        return GuardList([&] { return LocalizableTexturesImpl(a_nif); });
    }
    std::vector<std::string> ShapeNames(const std::vector<std::uint8_t>& a_nif) {
        return GuardList([&] { return ShapeNamesImpl(a_nif); });
    }
    std::vector<std::string> TexturePaths(const std::vector<std::uint8_t>& a_nif, int a_order) {
        return GuardList([&] { return TexturePathsImpl(a_nif, a_order); });
    }
    std::vector<std::pair<std::string, std::size_t>> ShapeVertexCounts(const std::vector<std::uint8_t>& a_nif) {
        try {
            NifFile     nif;
            std::string err;
            std::vector<std::pair<std::string, std::size_t>> out;
            if (!LoadBytes(nif, a_nif, err)) {
                return out;
            }
            for (auto* s : ShapesInOrder(nif)) {
                out.emplace_back(s->name.get(), static_cast<std::size_t>(s->GetNumVertices()));
            }
            return out;
        } catch (...) {
            return {};
        }
    }

    Result OffsetShapesImpl(const std::vector<std::uint8_t>& a_nif,
                            const std::map<std::string, std::vector<std::array<float, 3>>>& a_offsets) {
        Result  r;
        NifFile nif;
        if (!LoadBytes(nif, a_nif, r.error)) {
            return r;
        }
        const auto shapes = ShapesInOrder(nif);
        r.shapes          = static_cast<int>(shapes.size());
        for (const auto& [name, offsets] : a_offsets) {
            NiShape* target = nullptr;
            for (auto* s : shapes) {
                if (s->name.get() == name) {
                    target = s;
                    break;
                }
            }
            if (!target) {
                const auto want = Lower(name);
                int        hits = 0;
                for (auto* s : shapes) {
                    if (Lower(s->name.get()) == want) {
                        target = s;
                        ++hits;
                    }
                }
                if (hits != 1) {
                    target = nullptr;
                }
            }
            std::vector<Vector3> verts;
            if (!target || !nif.GetVertsForShape(target, verts) || verts.size() != offsets.size()) {
                ++r.unmatched;
                continue;
            }
            for (std::size_t i = 0; i < verts.size(); ++i) {
                verts[i].x += offsets[i][0];
                verts[i].y += offsets[i][1];
                verts[i].z += offsets[i][2];
            }
            nif.SetVertsForShape(target, verts);
            ++r.edited;
        }
        r.ok = SaveBytes(nif, r.bytes, r.error);
        return r;
    }

    Result OffsetShapes(const std::vector<std::uint8_t>& a_nif,
                        const std::map<std::string, std::vector<std::array<float, 3>>>& a_offsets) {
        return Guard([&] { return OffsetShapesImpl(a_nif, a_offsets); });
    }

    std::string VersionKey(const std::vector<std::uint8_t>& a_nif) {
        try {
            NifFile     nif;
            std::string err;
            if (!LoadBytes(nif, a_nif, err)) {
                return {};
            }
            const auto& v = nif.GetHeader().GetVersion();
            return std::to_string(static_cast<std::uint32_t>(v.File())) + "/" + std::to_string(v.User()) + "/" +
                   std::to_string(v.Stream());
        } catch (...) {
            return {};
        }
    }

    std::vector<std::array<float, 3>> Vertices(const std::vector<std::uint8_t>& a_nif, int a_order) {
        try {
            NifFile     nif;
            std::string err;
            std::vector<std::array<float, 3>> out;
            if (!LoadBytes(nif, a_nif, err)) {
                return out;
            }
            const auto shapes = ShapesInOrder(nif);
            if (a_order < 0 || a_order >= static_cast<int>(shapes.size())) {
                return out;
            }
            std::vector<Vector3> verts;
            if (nif.GetVertsForShape(shapes[static_cast<std::size_t>(a_order)], verts)) {
                for (const auto& v : verts) {
                    out.push_back({ v.x, v.y, v.z });
                }
            }
            return out;
        } catch (...) {
            return {};
        }
    }

}  // namespace OS::NifExport
