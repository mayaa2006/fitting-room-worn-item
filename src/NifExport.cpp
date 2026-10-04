#include "NifExport.h"

#include "ArmorExportModel.h"

#include <NifFile.hpp>

#include <functional>
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
            if (a_nif.Load(in) != 0 || !a_nif.IsValid()) {
                a_err = "not a NIF nifly can read";
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

    Result Rewrite(const std::vector<std::uint8_t>&          a_nif,
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
            if (!target && e.order >= 0 && e.order < static_cast<int>(shapes.size())) {
                target = shapes[static_cast<std::size_t>(e.order)];
            }
            if (!target) {
                ++r.unmatched;
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

        r.ok = SaveBytes(nif, r.bytes, r.error);
        return r;
    }

    Result Merge(const std::vector<std::vector<std::uint8_t>>& a_nifs) {
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
            if (LoadBytes(probe, r.bytes, err)) {
                r.shapes = static_cast<int>(ShapesInOrder(probe).size());
            }
            return r;
        }
        NifFile base;
        if (!LoadBytes(base, a_nifs.front(), r.error)) {
            r.error = "first model: " + r.error;
            return r;
        }
        std::set<std::string> taken;
        for (auto* s : ShapesInOrder(base)) {
            taken.insert(s->name.get());
        }
        for (std::size_t i = 1; i < a_nifs.size(); ++i) {
            NifFile     src;
            std::string err;
            if (!LoadBytes(src, a_nifs[i], err)) {
                r.error = "model " + std::to_string(i + 1) + ": " + err;
                return r;
            }
            for (auto* s : ShapesInOrder(src)) {
                std::string name = s->name.get();
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
            }
        }
        r.shapes = static_cast<int>(ShapesInOrder(base).size());
        r.ok     = SaveBytes(base, r.bytes, r.error);
        return r;
    }

    std::vector<std::string> ShapeNames(const std::vector<std::uint8_t>& a_nif) {
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

    std::vector<std::string> TexturePaths(const std::vector<std::uint8_t>& a_nif, int a_order) {
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

}  // namespace OS::NifExport
