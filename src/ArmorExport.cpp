#include "ArmorExport.h"

#include "BodyMeshPath.h"
#include "BodyMorphTri.h"
#include "EspWriter.h"
#include "NifExport.h"
#include "RendererData.h"
#include "StyleRef.h"
#include "TriFile.h"

// Zeroed-slider export reads BodySlide projects through Body Studio's loader,
// which only the default (FR_BODY_STUDIO) build carries.
#if defined(FR_BODY_STUDIO)
#    include "BodyMorphSource.h"
#    include "ZeroedModel.h"
#    include <tinyxml2.h>
#endif

#include <DirectXTex.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>

namespace OS::ArmorExport {

    namespace {
        namespace M  = ArmorExportModel;
        namespace fs = std::filesystem;
        using Microsoft::WRL::ComPtr;

        // ---- status ----------------------------------------------------------

        std::mutex g_statusLock;
        Status     g_status;

        void SetStatus(std::string a_msg) {
            std::scoped_lock l(g_statusLock);
            g_status.message = std::move(a_msg);
        }
        void SetCopying(bool a_on) {
            std::scoped_lock l(g_statusLock);
            g_status.copying = a_on;
        }
        void Finish(bool a_ok, std::string a_msg) {
            std::scoped_lock l(g_statusLock);
            g_status.busy     = false;
            g_status.copying  = false;
            g_status.finished = true;
            g_status.ok       = a_ok;
            g_status.message  = std::move(a_msg);
            if (a_ok) {
                spdlog::info("ArmorExport: {}", g_status.message);
            } else {
                spdlog::warn("ArmorExport: failed - {}", g_status.message);
            }
        }

        // ---- the job, as Gather leaves it ------------------------------------

        // One dyed texture to bake. The NiPointer keeps the forged texture (and
        // with it the GPU texture behind it) alive until the readback is done.
        struct Bake {
            RE::NiPointer<RE::NiSourceTexture> texture;
            std::string                        file;     // Data-relative output path
            std::string                        nifPath;  // as written into the NIF
            // Filled on the render thread:
            std::vector<std::vector<std::uint8_t>> mips;  // tightly packed RGBA8
            std::uint32_t                          width{ 0 };
            std::uint32_t                          height{ 0 };
            bool                                   srgb{ false };
            bool                                   read{ false };
        };

        // One displayed ARMA of a piece, with what its live 3D says.
        struct Addon {
            std::string                     model3rd;  // record-relative
            std::string                     model1st;  // record-relative, may be empty
            std::vector<NifExport::ShapeEdit> edits;
            std::string                     pieceName;  // for notes
            std::map<std::string, float>    morphs;     // baked into this piece's 3rd-person mesh
            EspWriter::FormRef              race;
            std::vector<EspWriter::FormRef> additionalRaces;
            std::uint32_t                   slots{ 0 };
            std::int8_t                     priority{ 0 };
            std::int8_t                     detection{ 0 };
            float                           weaponAdjust{ 0.0f };
        };

        struct Item {
            Bucket                         bucket{ Bucket::kTorso };
            ItemSettings                   settings;
            std::vector<Addon>             addons;
            std::uint32_t                  slots{ 0 };
            std::string                    worldModel;  // inventory/ground model of the main piece
            std::vector<EspWriter::FormRef> keywords;
            std::map<std::string, std::string> remap;  // texture key -> baked NIF path
        };

        struct Job {
            std::string              pluginName;  // file stem, no extension
            std::string              stem;        // folder stem
            bool                     recipe{ true };
            bool                     zeroed{ false };  // rebuild BodySlide pieces at zeroed sliders
            std::vector<Item>        items;
            std::vector<Bake>        bakes;
            std::vector<std::string> notes;
            EspWriter::FormRef       defaultRace;
            EspWriter::FormRef       forge;
            std::vector<std::string> masterOrder;
            int                      previews{ 0 };
        };

        // Readback handoff, one texture at a time: the worker names the bake it
        // wants and waits; the render-thread pump copies it and signals. One
        // texture in memory at a time, however many the outfit has.
        std::mutex              g_rbLock;
        std::condition_variable g_rbCv;
        Bake* g_rbWanted = nullptr;
        bool  g_rbDone   = false;
        bool  g_rbOk     = false;

        // ---- small helpers ---------------------------------------------------

        EspWriter::FormRef RefOf(const RE::TESForm* a_form) {
            if (!a_form) {
                return {};
            }
            auto* file = a_form->GetFile(0);
            if (!file) {
                return {};
            }
            return EspWriter::FormRef{ std::string(file->GetFilename()), a_form->GetLocalFormID(), false };
        }

        // Plugin strings are in the system ANSI code page (1252 on Western
        // installs, 1251 on Russian ones...); ImGui gives us UTF-8.
        std::string ToCp1252(const std::string& a_utf8) {
            if (a_utf8.empty()) {
                return a_utf8;
            }
            const int wlen = MultiByteToWideChar(CP_UTF8, 0, a_utf8.data(), static_cast<int>(a_utf8.size()),
                                                 nullptr, 0);
            if (wlen <= 0) {
                return a_utf8;
            }
            std::wstring w(static_cast<std::size_t>(wlen), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, a_utf8.data(), static_cast<int>(a_utf8.size()), w.data(), wlen);
            const int len = WideCharToMultiByte(CP_ACP, 0, w.data(), wlen, nullptr, 0, "?", nullptr);
            if (len <= 0) {
                return a_utf8;
            }
            std::string out(static_cast<std::size_t>(len), '\0');
            WideCharToMultiByte(CP_ACP, 0, w.data(), wlen, out.data(), len, "?", nullptr);
            return out;
        }

        std::string ModelOf(const RE::TESModel& a_model) {
            const char* m = a_model.GetModel();
            return (m && *m) ? std::string(m) : std::string{};
        }

        // Written into the mesh folder when an export starts and removed when
        // it finishes: a folder still holding it is what a failed or
        // interrupted export left behind, and the only kind ever cleared.
        constexpr const char* kInProgressFile = "FittingRoomExport.inprogress";

        std::vector<std::uint8_t> ArchiveBytes(const std::string& a_path) {
            RE::BSResourceNiBinaryStream stream{ a_path.c_str() };
            if (!stream.good() || !stream.stream) {
                return {};
            }
            const std::size_t total = stream.stream->totalSize;
            if (total == 0 || total > 256ull * 1024ull * 1024ull) {
                return {};
            }
            std::vector<std::uint8_t> bytes(total);
            if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::uint32_t>(total))) {
                return {};
            }
            return bytes;
        }

        bool WriteFile(const fs::path& a_path, const std::vector<std::uint8_t>& a_bytes, std::string& a_err) {
            std::error_code ec;
            fs::create_directories(a_path.parent_path(), ec);
            const auto tmp = a_path.string() + ".tmp";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out) {
                    a_err = "cannot write " + a_path.string();
                    return false;
                }
                out.write(reinterpret_cast<const char*>(a_bytes.data()),
                          static_cast<std::streamsize>(a_bytes.size()));
                if (!out) {
                    a_err = "write failed for " + a_path.string();
                    return false;
                }
            }
            fs::rename(tmp, a_path, ec);
            if (ec) {
                fs::remove(tmp, ec);
                a_err = "cannot replace " + a_path.string() + " (is it in use by the game?)";
                return false;
            }
            return true;
        }

        // ---- Gather: main thread ---------------------------------------------

        // The live lighting material of a geometry, or null.
        RE::BSLightingShaderMaterialBase* MaterialOf(RE::BSGeometry* a_geom) {
            auto* prop = netimmerse_cast<RE::BSLightingShaderProperty*>(
                a_geom->GetGeometryRuntimeData().shaderProperty.get());
            if (!prop || !prop->material ||
                prop->material->GetType() != RE::BSShaderMaterial::Type::kLighting) {
                return nullptr;
            }
            return static_cast<RE::BSLightingShaderMaterialBase*>(prop->material);
        }

        // Records one piece's displayed ARMA: model paths, and per shape the
        // dyed textures to bake and the slots they replace.
        void GatherAddon(Job& a_job, Item& a_item, RE::TESObjectARMA* a_arma, const RE::BIPOBJECT& a_obj,
                         RE::SEX a_sex, const std::string& a_pieceName,
                         const std::map<std::string, float>& a_morphs) {
            Addon ad;
            ad.pieceName = a_pieceName;
            for (const auto& [name, value] : a_morphs) {
                if (value != 0.0f) {
                    ad.morphs.emplace(name, value);
                }
            }
            const auto other = a_sex == RE::SEX::kFemale ? RE::SEX::kMale : RE::SEX::kFemale;
            ad.model3rd = a_obj.part ? ModelOf(*a_obj.part) : std::string{};
            if (ad.model3rd.empty()) {
                ad.model3rd = ModelOf(a_arma->bipedModels[a_sex]);
            }
            if (ad.model3rd.empty()) {
                ad.model3rd = ModelOf(a_arma->bipedModels[other]);
            }
            ad.model1st = ModelOf(a_arma->bipedModel1stPersons[a_sex]);
            if (ad.model1st.empty()) {
                ad.model1st = ModelOf(a_arma->bipedModel1stPersons[other]);
            }
            ad.race = RefOf(a_arma->race);
            for (auto* r : a_arma->additionalRaces) {
                if (auto ref = RefOf(r); !ref.Empty()) {
                    ad.additionalRaces.push_back(std::move(ref));
                }
            }
            ad.slots        = a_arma->GetSlotMask().underlying();
            ad.priority     = a_arma->data.priorities[a_sex];
            ad.detection    = a_arma->data.detectionSoundValue;
            ad.weaponAdjust = a_arma->data.weaponAdjust;
            const bool altTextures = a_arma->bipedModels[a_sex].numAlternateTextures > 0;

            int order = -1;
            RE::BSVisit::TraverseScenegraphGeometries(
                a_obj.partClone.get(), [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                    const char* gname = a_geom->name.c_str();
                    if (gname && M::IsInjectedOverlayName(gname)) {
                        return RE::BSVisit::BSVisitControl::kContinue;  // not in the mesh file
                    }
                    ++order;
                    // A part hidden in the outfit (or by anything else) is
                    // left out of the exported mesh: export what you see.
                    if (a_geom->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
                        NifExport::ShapeEdit rm;
                        rm.name   = a_geom->name.c_str() ? a_geom->name.c_str() : "";
                        rm.order  = order;
                        rm.remove = true;
                        ad.edits.push_back(std::move(rm));
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    auto* mat = MaterialOf(a_geom);
                    if (!mat) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    const auto feature = mat->GetFeature();
                    const bool skin    = feature == RE::BSShaderMaterial::Feature::kFaceGen ||
                                      feature == RE::BSShaderMaterial::Feature::kFaceGenRGBTint ||
                                      feature == RE::BSShaderMaterial::Feature::kHairTint;
                    std::array<std::string, 9> live{};
                    if (mat->textureSet) {
                        for (std::uint32_t s = 0; s < 9; ++s) {
                            const char* p = mat->textureSet->GetTexturePath(
                                static_cast<RE::BSTextureSet::Texture>(s));
                            live[s] = p ? p : "";
                        }
                    }
                    NifExport::ShapeEdit e;
                    e.name  = a_geom->name.c_str() ? a_geom->name.c_str() : "";
                    e.order = order;
                    // Alternate textures from the record are baked into the
                    // copied NIF, since the new ARMA carries none.
                    if (altTextures && !skin) {
                        for (std::uint32_t s = 0; s < 9; ++s) {
                            if (!live[s].empty()) {
                                e.textures[s] = M::NifTexturePath(live[s]);
                            }
                        }
                    }

                    // The dye swaps whole NiSourceTextures on the material;
                    // each one it made is named by its cache key.
                    struct Candidate {
                        RE::NiSourceTexture* tex;
                        std::uint32_t        fallbackSlot;
                    };
                    std::vector<Candidate> cands{ { mat->diffuseTexture.get(), 0 },
                                                  { mat->normalTexture.get(), 1 } };
                    if (feature == RE::BSShaderMaterial::Feature::kGlowMap) {
                        cands.push_back({ static_cast<RE::BSLightingShaderMaterialGlowmap*>(mat)->glowTexture.get(), 2 });
                    }
                    if (feature == RE::BSShaderMaterial::Feature::kEnvironmentMap) {
                        cands.push_back(
                            { static_cast<RE::BSLightingShaderMaterialEnvmap*>(mat)->envMaskTexture.get(), 5 });
                    }
                    bool dyed = false;
                    for (const auto& c : cands) {
                        if (!c.tex) {
                            continue;
                        }
                        const std::string name = c.tex->name.c_str() ? c.tex->name.c_str() : "";
                        if (!M::IsDyeTextureName(name)) {
                            continue;
                        }
                        if (M::IsDyePreviewName(name)) {
                            ++a_job.previews;
                        }
                        const auto identity = M::DyeIdentity(name);
                        const auto key      = M::TextureKey(identity);
                        // Which slot of the shape held that texture: by path,
                        // so the engine's slot naming never matters.
                        std::uint32_t slot = c.fallbackSlot;
                        for (std::uint32_t s = 0; s < 9; ++s) {
                            if (!live[s].empty() && M::TextureKey(live[s]) == key) {
                                slot = s;
                                break;
                            }
                        }
                        // One file per distinct GPU texture.
                        std::string nifPath;
                        for (const auto& b : a_job.bakes) {
                            if (b.texture.get() == c.tex) {
                                nifPath = b.nifPath;
                                break;
                            }
                        }
                        if (nifPath.empty()) {
                            Bake b;
                            b.texture = RE::NiPointer<RE::NiSourceTexture>(c.tex);
                            const auto fileName = "t" + std::to_string(a_job.bakes.size() + 1) + ".dds";
                            b.nifPath = "textures\\FittingRoom\\Armor\\" + a_job.stem + "\\" + fileName;
                            b.file    = "Data\\" + b.nifPath;
                            nifPath   = b.nifPath;
                            a_job.bakes.push_back(std::move(b));
                        }
                        e.textures[slot] = nifPath;
                        if (!key.empty() && !a_item.remap.contains(key)) {
                            a_item.remap.emplace(key, nifPath);
                        }
                        dyed = true;
                    }
                    if (dyed) {
                        e.hasSpecular      = true;
                        e.specular[0]      = mat->specularColor.red;
                        e.specular[1]      = mat->specularColor.green;
                        e.specular[2]      = mat->specularColor.blue;
                        e.glossiness       = mat->specularPower;
                        e.specularStrength = mat->specularColorScale;
                    }
                    if (!e.textures.empty() || e.hasSpecular) {
                        ad.edits.push_back(std::move(e));
                    }
                    return RE::BSVisit::BSVisitControl::kContinue;
                });

            if (ad.model3rd.empty()) {
                a_job.notes.push_back("a piece had no model path and was left out");
                return;
            }
            a_item.slots |= ad.slots;
            a_item.addons.push_back(std::move(ad));
        }

        void Worker(std::shared_ptr<Job> a_job);

        void Gather(Request a_req) {
            auto job        = std::make_shared<Job>();
            job->pluginName = M::SafeName(M::PluginStem(a_req.pluginName), 48, "Fitting Room Armor");
            // Short on purpose: it is a folder in every exported path, and
            // MO2 / Wabbajack installs already eat much of Windows' 260.
            job->stem       = M::SafeStem(job->pluginName, 16, "Outfit") + "_" +
                        M::HashHex(job->pluginName).substr(0, 6);
            job->recipe     = a_req.recipe;
            job->zeroed     = a_req.zeroed;

            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* base   = player ? player->GetActorBase() : nullptr;
            if (!player || !base || !player->Get3D()) {
                Finish(false, "Your character is not loaded.");
                return;
            }
            const auto& bipedPtr = player->GetBiped(false);
            auto*       biped    = bipedPtr.get();
            if (!biped) {
                Finish(false, "Could not read what your character is wearing.");
                return;
            }
            if (auto* dh = RE::TESDataHandler::GetSingleton();
                dh && dh->LookupLoadedModByName(job->pluginName + ".esp")) {
                Finish(false, "\"" + job->pluginName +
                                  ".esp\" is loaded right now, so it cannot be replaced. Pick a new name.");
                return;
            }
            {
                // Never overwrite an earlier export: its items may be in saves.
                std::error_code ec;
                const auto meshDir = fs::path("Data\\meshes\\FittingRoom\\Armor") / job->stem;
                const auto texDir  = fs::path("Data\\textures\\FittingRoom\\Armor") / job->stem;
                if (fs::exists(fs::path("Data") / (job->pluginName + ".esp"), ec)) {
                    Finish(false, "\"" + job->pluginName + ".esp\" already exists. Pick a new plugin name.");
                    return;
                }
                if (fs::exists(meshDir / kInProgressFile, ec)) {
                    // Left by a failed or interrupted run of this same name.
                    fs::remove_all(meshDir, ec);
                    fs::remove_all(texDir, ec);
                } else if (fs::exists(meshDir, ec) || fs::exists(texDir, ec)) {
                    Finish(false, "Files from an earlier export with this name are still in Data (\"" +
                                      job->stem + "\"). Pick a new plugin name.");
                    return;
                }
            }
            const auto sex = base->GetSex() == RE::SEX::kFemale ? RE::SEX::kFemale : RE::SEX::kMale;

            for (auto& [plan, settings] : a_req.items) {
                if (!settings.include) {
                    continue;
                }
                Item item;
                item.bucket   = plan.bucket;
                item.settings = settings;
                RE::TESObjectARMO* mainArmo = nullptr;
                for (std::size_t si = 0; si < plan.styles.size(); ++si) {
                    const auto& key = plan.styles[si];
                    static const std::map<std::string, float> kNoMorphs;
                    const auto& morphs = si < settings.morphs.size() ? settings.morphs[si] : kNoMorphs;
                    auto* armo = StyleRef::Resolve(key);
                    if (!armo) {
                        job->notes.push_back("a piece from " + key.modName + " is not in your load order");
                        continue;
                    }
                    std::set<RE::TESObjectARMA*> done;
                    bool                         any = false;
                    for (std::uint32_t bit = 0; bit < 32; ++bit) {
                        const auto& obj = biped->objects[bit];
                        if (!obj.addon || !obj.partClone || done.contains(obj.addon)) {
                            continue;
                        }
                        bool mine = false;
                        for (auto* aa : armo->armorAddons) {
                            mine = mine || aa == obj.addon;
                        }
                        if (!mine) {
                            continue;
                        }
                        done.insert(obj.addon);
                        const char* pn = armo->GetName();
                        GatherAddon(*job, item, obj.addon, obj, sex, (pn && *pn) ? pn : "a piece", morphs);
                        any = true;
                    }
                    if (!any) {
                        const char* n = armo->GetName();
                        job->notes.push_back(std::string("\"") + (n && *n ? n : "a piece") +
                                             "\" is not showing on your character, so it was left out");
                        continue;
                    }
                    if (!mainArmo) {
                        mainArmo = armo;
                    }
                }
                if (item.addons.empty()) {
                    continue;
                }
                // Jewelry merged into a torso keeps its mesh but not its slots:
                // otherwise the cuirass would unequip your real amulet and
                // rings and count as a necklace at the enchanter.
                if (item.bucket == Bucket::kTorso && (item.slots & M::kBodyBit)) {
                    item.slots &= ~(M::kAmuletBit | M::kRingBit);
                }
                // The body piece is the merge's base, so its root extra data
                // (body morph and physics links) is the one that survives.
                std::stable_partition(item.addons.begin(), item.addons.end(),
                                      [](const Addon& a) { return (a.slots & M::kBodyBit) != 0; });
                if (mainArmo) {
                    item.worldModel = ModelOf(mainArmo->worldModels[sex]);
                    if (item.worldModel.empty()) {
                        item.worldModel = ModelOf(mainArmo->worldModels[sex == RE::SEX::kFemale ? RE::SEX::kMale
                                                                                                : RE::SEX::kFemale]);
                    }
                }
                for (const auto& edid : M::KeywordEditorIds(item.slots, item.settings.type)) {
                    if (auto* kw = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(edid)) {
                        if (auto ref = RefOf(kw); !ref.Empty()) {
                            item.keywords.push_back(std::move(ref));
                        }
                    }
                }
                job->items.push_back(std::move(item));
            }
            if (job->items.empty()) {
                std::string why = "Nothing to export: none of the outfit's pieces are showing on your character.";
                if (!job->notes.empty()) {
                    why += " (" + job->notes.front() + ")";
                }
                Finish(false, why);
                return;
            }

            job->defaultRace = RefOf(RE::TESForm::LookupByID<RE::TESRace>(0x19));
            job->forge       = RefOf(RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingSmithingForge"));

            // Master order = load order, so xEdit has nothing to complain about.
            if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                std::vector<std::pair<std::uint32_t, std::string>> ranked;
                std::set<std::string>                              seen;
                auto note = [&](const EspWriter::FormRef& r) {
                    if (r.Empty() || r.own || !seen.insert(r.file).second) {
                        return;
                    }
                    const auto* f = dh->LookupLoadedModByName(r.file);
                    ranked.emplace_back(f ? f->GetPartialIndex() : 0xFFFFFFFFu, r.file);
                };
                note(job->defaultRace);
                note(job->forge);
                for (const auto& it : job->items) {
                    for (const auto& k : it.keywords) note(k);
                    for (const auto& a : it.addons) {
                        note(a.race);
                        for (const auto& r : a.additionalRaces) note(r);
                    }
                }
                std::stable_sort(ranked.begin(), ranked.end(),
                                 [](const auto& x, const auto& y) { return x.first < y.first; });
                for (auto& [rank, name] : ranked) {
                    job->masterOrder.push_back(std::move(name));
                }
            }

            if (job->previews > 0) {
                // A 512 px preview means a colour is still settling; the full
                // size build lands a moment later.
                for (auto& b : job->bakes) {
                    b.texture.reset();
                }
                Finish(false, "The dyes are still finishing. Wait a second and try again.");
                return;
            }

            SetStatus(job->bakes.empty() ? "Writing files..." : "Copying the dyed textures...");
            std::thread([job] { Worker(job); }).detach();
        }

        // ---- Worker ----------------------------------------------------------

        bool WriteDds(Bake& a_b, std::string& a_err) {
            const auto fmt = a_b.srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
            DirectX::ScratchImage raw;
            if (FAILED(raw.Initialize2D(fmt, a_b.width, a_b.height, 1, a_b.mips.size()))) {
                a_err = "could not allocate a texture";
                return false;
            }
            for (std::size_t m = 0; m < a_b.mips.size(); ++m) {
                const auto* img = raw.GetImage(m, 0, 0);
                if (!img) {
                    a_err = "texture mip missing";
                    return false;
                }
                const std::size_t row = static_cast<std::size_t>(img->width) * 4u;
                for (std::size_t y = 0; y < img->height; ++y) {
                    const auto src = y * row;
                    if (src + row > a_b.mips[m].size()) {
                        break;
                    }
                    std::memcpy(img->pixels + y * img->rowPitch, a_b.mips[m].data() + src, row);
                }
            }
            DirectX::ScratchImage bc;
            const auto bcFmt = a_b.srgb ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
            HRESULT    hr    = DirectX::Compress(raw.GetImages(), raw.GetImageCount(), raw.GetMetadata(), bcFmt,
                                                 DirectX::TEX_COMPRESS_BC7_QUICK | DirectX::TEX_COMPRESS_PARALLEL,
                                                 DirectX::TEX_THRESHOLD_DEFAULT, bc);
            const DirectX::ScratchImage& out = SUCCEEDED(hr) ? bc : raw;
            if (FAILED(hr)) {
                spdlog::warn("ArmorExport: BC7 compression failed ({:08X}); writing uncompressed.",
                             static_cast<std::uint32_t>(hr));
            }
            std::error_code ec;
            const fs::path  path(a_b.file);
            fs::create_directories(path.parent_path(), ec);
            const auto tmp = fs::path(path.wstring() + L".tmp");
            hr = DirectX::SaveToDDSFile(out.GetImages(), out.GetImageCount(), out.GetMetadata(),
                                        DirectX::DDS_FLAGS_NONE, tmp.wstring().c_str());
            if (FAILED(hr)) {
                fs::remove(tmp, ec);
                a_err = "could not write " + a_b.file;
                return false;
            }
            fs::rename(tmp, path, ec);
            if (ec) {
                fs::remove(tmp, ec);
                a_err = "could not place " + a_b.file;
                return false;
            }
            return true;
        }

        // ---- zeroed sliders: rebuild BodySlide pieces from their projects ----

        struct ZeroProject {
            std::string project;  // the .osp
            std::string set;      // the slider set in it
        };

        struct ZeroPiece {
            bool                         ok{ false };
            std::vector<std::uint8_t>    bytes;  // the zero build
            std::optional<TriFile::File> tri;    // its body-slider file
            std::string                  why;    // when not ok
        };

        struct ZeroContext {
            std::unordered_multimap<std::string, ZeroProject> index;  // mesh key -> sets that build it
            std::map<std::string, ZeroPiece>                  cache;  // mesh key -> result
        };

#if defined(FR_BODY_STUDIO)
        std::vector<std::uint8_t> ReadLoose(const fs::path& a_path) {
            std::error_code ec;
            const auto      size = fs::file_size(a_path, ec);
            if (ec || size == 0 || size > 256ull * 1024ull * 1024ull) {
                return {};
            }
            std::ifstream             in(a_path, std::ios::binary);
            std::vector<std::uint8_t> out(static_cast<std::size_t>(size));
            if (!in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size))) {
                return {};
            }
            return out;
        }

        // Every slider set BodySlide knows, by the mesh it builds.
        void BuildZeroIndex(ZeroContext& a_ctx) {
            const fs::path  root{ "Data/CalienteTools/BodySlide/SliderSets" };
            std::error_code ec;
            if (!fs::is_directory(root, ec)) {
                return;
            }
            std::size_t files = 0;
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec)) {
                    continue;
                }
                auto ext = it->path().extension().string();
                for (auto& c : ext) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (ext != ".osp" && ext != ".xml") {
                    continue;
                }
                const auto bytes = ReadLoose(it->path());
                if (bytes.empty()) {
                    continue;
                }
                tinyxml2::XMLDocument doc;
                if (doc.Parse(reinterpret_cast<const char*>(bytes.data()), bytes.size()) != tinyxml2::XML_SUCCESS) {
                    continue;
                }
                auto* info = doc.FirstChildElement("SliderSetInfo");
                if (!info) {
                    continue;
                }
                ++files;
                for (auto* set = info->FirstChildElement("SliderSet"); set; set = set->NextSiblingElement("SliderSet")) {
                    const char* name = set->Attribute("name");
                    auto*       file = set->FirstChildElement("OutputFile");
                    if (!name || !*name || !file || !file->GetText()) {
                        continue;
                    }
                    auto*       dir  = set->FirstChildElement("OutputPath");
                    const char* dirT = dir && dir->GetText() ? dir->GetText() : "";
                    auto        key  = BodyMeshKey(dirT, file->GetText());
                    if (!key.empty()) {
                        a_ctx.index.emplace(std::move(key), ZeroProject{ it->path().string(), name });
                    }
                }
            }
            spdlog::info("ArmorExport: {} BodySlide project file(s), {} slider set(s) indexed for zeroed export.",
                         files, a_ctx.index.size());
        }

        // The zero build of one built mesh, from whichever slider set builds
        // it and matches it shape for shape. Cached per mesh (both weights).
        const ZeroPiece& ZeroFor(ZeroContext& a_ctx, const std::string& a_modelPath,
                                 const std::vector<std::uint8_t>& a_built) {
            const auto key = BodyMeshKey(a_modelPath);
            if (const auto it = a_ctx.cache.find(key); it != a_ctx.cache.end()) {
                return it->second;
            }
            ZeroPiece& out = a_ctx.cache[key];
            const auto range = a_ctx.index.equal_range(key);
            if (range.first == range.second) {
                out.why = "not built by BodySlide";
                return out;
            }
            const auto builtVersion = NifExport::VersionKey(a_built);
            std::map<std::string, std::size_t> builtCounts;
            for (auto& [n, c] : NifExport::ShapeVertexCounts(a_built)) {
                builtCounts.emplace(n, c);
            }

            int                                best = -1;
            BodyMorphSource::Source            bestSrc;
            std::vector<std::uint8_t>          bestBase;
            std::map<std::string, std::size_t> bestCounts;
            std::string                        lastWhy = "its BodySlide project could not be read";
            for (auto it = range.first; it != range.second; ++it) {
                BodyMorphSource::Source src;
                if (!BodyMorphSource::Load(it->second.project, it->second.set, src)) {
                    continue;
                }
                const auto base = ReadLoose(fs::path("Data") / src.referenceMesh);
                if (base.empty()) {
                    lastWhy = "its BodySlide reference mesh is missing";
                    continue;
                }
                if (NifExport::VersionKey(base) != builtVersion) {
                    lastWhy = "its BodySlide project is for another game version";
                    continue;
                }
                std::map<std::string, std::size_t> counts;
                for (auto& [n, c] : NifExport::ShapeVertexCounts(base)) {
                    counts.emplace(n, c);
                }
                // Every built shape must be in the project's mesh, and no
                // smaller (a zap can only have taken vertices away).
                int  score = 0;
                bool fits  = true;
                for (const auto& [n, c] : builtCounts) {
                    const auto b = ZeroedModel::FindShape(counts, n);
                    if (b == counts.end() || b->second < c) {
                        fits = false;
                        break;
                    }
                    score += b->second == c ? 1 : 0;
                }
                if (!fits) {
                    lastWhy = "its BodySlide project does not match the built mesh";
                    continue;
                }
                if (score > best) {
                    best       = score;
                    bestSrc    = std::move(src);
                    bestBase   = base;
                    bestCounts = std::move(counts);
                    spdlog::info("ArmorExport: zeroed '{}' from set '{}' ({}), {} of {} shape(s) exact.",
                                 a_modelPath, it->second.set, it->second.project, score, builtCounts.size());
                }
            }
            if (best < 0) {
                out.why = lastWhy;
                return out;
            }
            for (const auto& zap : ZeroedModel::ZapsOnAtZero(bestSrc)) {
                spdlog::warn("ArmorExport: '{}': slider '{}' would delete geometry at zero; left off.", a_modelPath,
                             zap);
            }
            auto moved = NifExport::OffsetShapes(bestBase, ZeroedModel::ZeroOffsets(bestSrc, bestCounts));
            if (!moved.ok) {
                out.why = "rebuilding it failed (" + moved.error + ")";
                return out;
            }
            out.ok    = true;
            out.bytes = std::move(moved.bytes);
            auto tri  = ZeroedModel::TriFromSource(bestSrc, bestCounts);
            if (!tri.empty()) {
                out.tri = std::move(tri);
            }
            return out;
        }
#else
        void BuildZeroIndex(ZeroContext&) {}
        const ZeroPiece& ZeroFor(ZeroContext& a_ctx, const std::string& a_modelPath,
                                 const std::vector<std::uint8_t>&) {
            ZeroPiece& out = a_ctx.cache[BodyMeshKey(a_modelPath)];
            out.why        = "this build has no BodySlide project reader";
            return out;
        }
#endif

        // A tri file named by a mesh (BODYTRI is meshes-relative), parsed.
        std::optional<TriFile::File> ReadTri(const std::string& a_meshesRel) {
            if (a_meshesRel.empty()) {
                return std::nullopt;
            }
            const auto bytes = ArchiveBytes(M::ArchiveMeshPath(M::RecordModelPath(a_meshesRel)));
            if (bytes.empty()) {
                return std::nullopt;
            }
            return TriFile::Parse(bytes);
        }

        // One NIF for an item: every addon's model rewritten, then merged.
        // a_variant is "_0", "_1" or "" (single file); a_firstPerson picks the
        // first-person models instead. Returns false with a_err on failure, or
        // true with empty a_out when no addon has such a model.
        //
        // Third person also bakes each piece's own morph values into it, and
        // when any piece follows body sliders, gathers the pieces' tri data
        // under their merged shape names into a_triOut and links the merged
        // mesh to a_triRel - one tri per item, so the merged armor still
        // follows the wearer's body sliders.
        bool BuildNif(const Item& a_item, const std::string& a_variant, bool a_firstPerson,
                      const std::string& a_triRel, std::vector<std::uint8_t>& a_out,
                      std::vector<std::uint8_t>& a_triOut, std::vector<std::string>& a_notes, std::string& a_err,
                      ZeroContext* a_zero) {
            a_triOut.clear();
            // Notes once per item, not once per weight variant.
            const bool report = !a_firstPerson && a_variant != "_0";
            std::vector<std::vector<std::uint8_t>> parts;
            struct PartTri {
                bool                         linked{ false };  // the mesh named a tri
                std::optional<TriFile::File> tri;              // ... and it was readable
            };
            std::vector<PartTri> partTris;
            for (const auto& ad : a_item.addons) {
                const auto& model = a_firstPerson ? ad.model1st : ad.model3rd;
                if (model.empty()) {
                    continue;
                }
                const auto variants = M::MeshVariants(model);
                std::string path    = variants.front().path;
                for (const auto& v : variants) {
                    if (v.suffix == a_variant) {
                        path = v.path;
                    }
                }
                auto bytes = ArchiveBytes(M::ArchiveMeshPath(path));
                if (bytes.empty() && path != M::RecordModelPath(model)) {
                    bytes = ArchiveBytes(M::ArchiveMeshPath(model));  // the other weight is missing
                }
                if (bytes.empty()) {
                    a_notes.push_back("could not read meshes\\" + M::RecordModelPath(path));
                    continue;
                }
                // Zeroed sliders: the same piece rebuilt from its BodySlide
                // project, in place of whatever preset built the file.
                const ZeroPiece* zero = nullptr;
                if (a_zero) {
                    zero = &ZeroFor(*a_zero, M::RecordModelPath(path), bytes);
                    if (zero->ok) {
                        bytes = zero->bytes;
                    }
                }
                // Third person: the per-shape edits only, which are exact.
                // First person has no live shapes to match, so it takes the
                // path remap instead (a texture the third-person bake
                // replaced is replaced here too).
                static const std::vector<NifExport::ShapeEdit>   kNoEdits;
                static const std::map<std::string, std::string> kNoRemap;
                auto r = NifExport::Rewrite(bytes, a_firstPerson ? kNoEdits : ad.edits,
                                            a_firstPerson ? a_item.remap : kNoRemap);
                if (!r.ok) {
                    a_notes.push_back("meshes\\" + M::RecordModelPath(path) + ": " + r.error);
                    continue;
                }
                if (r.unmatched > 0) {
                    spdlog::warn("ArmorExport: {} shape edit(s) found no shape in {}.", r.unmatched, path);
                }
                PartTri pt;
                if (!a_firstPerson) {
                    std::optional<TriFile::File> bakeTri;
                    if (zero && zero->ok) {
                        // The zero build's own sliders, rebuilt from the
                        // project on its own vertices.
                        pt.linked = zero->tri.has_value();
                        pt.tri    = zero->tri;
                        bakeTri   = zero->tri;
                    } else {
                        const auto linkPath = NifExport::BodyTriPath(r.bytes);
                        pt.linked           = !linkPath.empty();
                        pt.tri              = ReadTri(linkPath);
                        // The menu listed the morphs of the tri BodySlide wrote
                        // beside the mesh; use that one when the mesh names none.
                        bakeTri = pt.tri;
                        if (!bakeTri && !pt.linked) {
                            bakeTri = ReadTri(BodyMorphTri::TriPathFor(M::RecordModelPath(model)));
                        }
                    }
                    if (!ad.morphs.empty()) {
                        if (!bakeTri) {
                            if (report) {
                                a_notes.push_back("\"" + ad.pieceName +
                                                  "\" has no body-slider data, so its morph changes were not applied");
                            }
                        } else {
                            auto m = NifExport::ApplyMorphs(r.bytes, *bakeTri, ad.morphs);
                            if (!m.ok) {
                                if (report) {
                                    a_notes.push_back("\"" + ad.pieceName + "\" morphs: " + m.error);
                                }
                            } else {
                                if (m.edited == 0 && report) {
                                    a_notes.push_back("the morph changes on \"" + ad.pieceName +
                                                      "\" did not move any of its shapes");
                                }
                                spdlog::info("ArmorExport: baked {} morph value(s) into {} shape(s) of {}.",
                                             ad.morphs.size(), m.edited, path);
                                r.bytes = std::move(m.bytes);
                            }
                        }
                    }
                }
                parts.push_back(std::move(r.bytes));
                partTris.push_back(std::move(pt));
            }
            if (parts.empty()) {
                a_out.clear();
                return true;
            }
            auto merged = NifExport::Merge(parts);
            if (!merged.ok) {
                a_err = std::string(M::BucketLabel(a_item.bucket)) + " merge: " + merged.error;
                return false;
            }
            if (merged.droppedPhysics > 0 && report) {
                a_notes.push_back(std::to_string(merged.droppedPhysics) + " piece(s) merged into the " +
                                  M::BucketLabel(a_item.bucket) +
                                  " had physics that one merged mesh cannot keep; they will not move with physics");
            }

            if (!a_firstPerson && !a_triRel.empty()) {
                TriFile::File combined;
                int           lost = 0;
                for (std::size_t i = 0; i < partTris.size() && i < merged.names.size(); ++i) {
                    const auto& pt = partTris[i];
                    if (!pt.linked) {
                        continue;
                    }
                    if (!pt.tri) {
                        ++lost;
                        continue;
                    }
                    for (auto& ts : TriFile::Renamed(*pt.tri, merged.names[i])) {
                        combined.push_back(std::move(ts));
                    }
                }
                if (!combined.empty()) {
                    auto linked = NifExport::SetBodyTri(merged.bytes, a_triRel);
                    if (!linked.ok) {
                        a_err = std::string(M::BucketLabel(a_item.bucket)) + " body sliders: " + linked.error;
                        return false;
                    }
                    merged.bytes = std::move(linked.bytes);
                    a_triOut     = TriFile::Serialize(combined);
                }
                if (lost > 0 && report) {
                    a_notes.push_back(std::to_string(lost) + " piece(s) in the " + M::BucketLabel(a_item.bucket) +
                                      " link body-slider data that could not be read; they will not follow body sliders");
                }
            }
            a_out = std::move(merged.bytes);
            return true;
        }

        // Self-contained export: copies every armour texture a mesh uses into
        // the export folder (under src\, with short hashed names)
        // and points the mesh at the copies. Baked dyes are already there.
        // Skin shapes are left alone by NifExport, so the game still paints
        // them with the wearer's skin. a_copied is shared across the export so
        // each texture is copied once.
        bool LocalizeTextures(Job& a_job, std::map<std::string, std::string>& a_copied,
                              std::vector<std::uint8_t>& a_bytes, std::string& a_err) {
            const std::string ownRoot = "textures\\FittingRoom\\Armor\\" + a_job.stem + "\\";
            const std::string ownKey  = M::TextureKey(ownRoot);
            std::map<std::string, std::string> map;
            for (const auto& path : NifExport::LocalizableTextures(a_bytes)) {
                const auto key = M::TextureKey(path);
                if (key.empty() || key.starts_with(ownKey)) {
                    continue;  // empty slot, or one of our baked dyes
                }
                auto it = a_copied.find(key);
                if (it == a_copied.end()) {
                    const auto src = ArchiveBytes("textures\\" + key);
                    std::string dst;
                    if (src.empty()) {
                        a_job.notes.push_back("could not copy textures\\" + key +
                                              ", so the mesh still uses the original");
                    } else {
                        dst = ownRoot + "src\\" + M::CopiedTextureFileName(key);
                        if (!WriteFile(fs::path("Data\\" + dst), src, a_err)) {
                            return false;
                        }
                    }
                    it = a_copied.emplace(key, dst).first;
                }
                if (!it->second.empty()) {
                    map.emplace(key, it->second);
                }
            }
            if (map.empty()) {
                return true;
            }
            auto r = NifExport::Localize(a_bytes, map);
            if (!r.ok) {
                a_err = "texture copy: " + r.error;
                return false;
            }
            a_bytes = std::move(r.bytes);
            return true;
        }

        // Worker side of the readback handoff. False if the render thread did
        // not get to it in time (the pump only runs while FLICK renders).
        bool RequestReadback(Bake& a_b) {
            std::unique_lock l(g_rbLock);
            g_rbWanted = &a_b;
            g_rbDone   = false;
            g_rbOk     = false;
            if (!g_rbCv.wait_for(l, std::chrono::seconds(20), [] { return g_rbDone; })) {
                g_rbWanted = nullptr;
                spdlog::warn("ArmorExport: the render thread never picked up {}.", a_b.file);
                return false;
            }
            return g_rbOk;
        }

        // A failed export removes everything it wrote, so nothing half-made is
        // left in Data and the same name can be used again.
        void Fail(const Job& a_job, std::string a_msg) {
            std::error_code ec;
            fs::remove_all(fs::path("Data\\meshes\\FittingRoom\\Armor") / a_job.stem, ec);
            fs::remove_all(fs::path("Data\\textures\\FittingRoom\\Armor") / a_job.stem, ec);
            Finish(false, std::move(a_msg));
        }

        void Worker(std::shared_ptr<Job> a_job) {
            auto& job = *a_job;
            {
                std::error_code ec;
                const auto      meshDir = fs::path("Data\\meshes\\FittingRoom\\Armor") / job.stem;
                fs::create_directories(meshDir, ec);
                std::ofstream mark(meshDir / kInProgressFile);
                mark << job.pluginName << ".esp\n";
            }
            try {
                // 1. Textures, one at a time: read back on the render thread,
                // compress and write here, free, next.
                int                   n = 0;
                std::set<std::string> failed;  // NIF paths of bakes that did not land
                SetCopying(!job.bakes.empty());
                for (auto& b : job.bakes) {
                    ++n;
                    const auto of = std::to_string(n) + " of " + std::to_string(job.bakes.size());
                    SetStatus("Copying dyed texture " + of + "...");
                    b.read = RequestReadback(b);
                    std::string err;
                    if (!b.read) {
                        failed.insert(b.nifPath);
                        job.notes.push_back("a dyed texture could not be copied and stays undyed");
                    } else {
                        SetStatus("Compressing dyed texture " + of + "...");
                        if (!WriteDds(b, err)) {
                            failed.insert(b.nifPath);
                            job.notes.push_back(err);
                        }
                    }
                    b.mips.clear();
                    b.mips.shrink_to_fit();
                }
                // The textures were only held for the copy. Let them go on the
                // game thread, where every other reference to them is dropped.
                {
                    std::vector<RE::NiPointer<RE::NiSourceTexture>> held;
                    for (auto& b : job.bakes) {
                        held.push_back(std::move(b.texture));
                    }
                    if (auto* task = SKSE::GetTaskInterface()) {
                        task->AddTask([h = std::move(held)]() mutable { h.clear(); });
                    }
                }
                SetCopying(false);
                // A mesh must never name a texture that was not written: drop
                // those slots, so they keep the original (undyed) texture.
                if (!failed.empty()) {
                    for (auto& item : job.items) {
                        for (auto& ad : item.addons) {
                            for (auto& e : ad.edits) {
                                std::erase_if(e.textures, [&](const auto& kv) { return failed.contains(kv.second); });
                            }
                        }
                        std::erase_if(item.remap, [&](const auto& kv) { return failed.contains(kv.second); });
                    }
                }

                // 2. Meshes.
                std::unique_ptr<ZeroContext> zeroCtx;
                if (job.zeroed) {
                    SetStatus("Reading your BodySlide projects...");
                    zeroCtx = std::make_unique<ZeroContext>();
                    BuildZeroIndex(*zeroCtx);
                }
                const std::string meshRel = "FittingRoom\\Armor\\" + job.stem + "\\";
                EspWriter::PluginSpec spec;
                spec.author      = "Fitting Room";
                spec.description = "Made from the Fitting Room outfit export.";
                spec.masterOrder = job.masterOrder;
                std::uint32_t nextId = 0x800;
                std::map<std::string, std::string> copiedTextures;  // texture key -> copy (empty: failed)
                int           written = 0;
                for (auto& item : job.items) {
                    const std::string label = M::BucketLabel(item.bucket);
                    SetStatus("Building the " + label + " mesh...");
                    bool anyPair = false;
                    for (const auto& ad : item.addons) {
                        anyPair = anyPair || M::MeshVariants(ad.model3rd).size() == 2;
                    }
                    const std::vector<std::string> variants =
                        anyPair ? std::vector<std::string>{ "_0", "_1" } : std::vector<std::string>{ "" };
                    std::string modelPath;
                    std::string err;
                    std::size_t wroteVariants = 0;
                    const auto  triRel        = meshRel + label + ".tri";
                    std::vector<std::uint8_t> triBytes;
                    for (const auto& v : variants) {
                        std::vector<std::uint8_t> bytes;
                        std::vector<std::uint8_t> tri;
                        if (!BuildNif(item, v, false, triRel, bytes, tri, job.notes, err, zeroCtx.get())) {
                            Fail(job, err);
                            return;
                        }
                        if (bytes.empty()) {
                            break;
                        }
                        SetStatus("Copying the " + label + " textures...");
                        if (!LocalizeTextures(job, copiedTextures, bytes, err)) {
                            Fail(job, err);
                            return;
                        }
                        const auto rel = meshRel + label + v + ".nif";
                        if (!WriteFile(fs::path("Data\\meshes\\" + rel), bytes, err)) {
                            Fail(job, err);
                            return;
                        }
                        if (!tri.empty()) {
                            triBytes = std::move(tri);  // the weights share one tri, as BodySlide's do
                        }
                        ++wroteVariants;
                    }
                    if (!triBytes.empty() &&
                        !WriteFile(fs::path("Data\\meshes\\" + triRel), triBytes, err)) {
                        Fail(job, err);
                        return;
                    }
                    if (wroteVariants == variants.size()) {
                        modelPath = meshRel + label + (anyPair ? "_1" : "") + ".nif";
                    }
                    if (modelPath.empty()) {
                        job.notes.push_back(label + " had no readable mesh and was left out");
                        continue;
                    }
                    // First person, in the same weight variants as the third
                    // person model, since the record's weight slider covers both.
                    std::string first;
                    {
                        std::size_t wrote1st = 0;
                        for (const auto& v : variants) {
                            std::vector<std::uint8_t> bytes;
                            std::vector<std::uint8_t> noTri;
                            if (!BuildNif(item, v, true, {}, bytes, noTri, job.notes, err, zeroCtx.get())) {
                                Fail(job, err);
                                return;
                            }
                            if (bytes.empty()) {
                                break;
                            }
                            if (!LocalizeTextures(job, copiedTextures, bytes, err)) {
                                Fail(job, err);
                                return;
                            }
                            const auto rel = meshRel + label + "_1stPerson" + v + ".nif";
                            if (!WriteFile(fs::path("Data\\meshes\\" + rel), bytes, err)) {
                                Fail(job, err);
                                return;
                            }
                            ++wrote1st;
                        }
                        if (wrote1st == variants.size()) {
                            first = meshRel + label + "_1stPerson" + (anyPair ? "_1" : "") + ".nif";
                        }
                    }

                    // 3. Records.
                    const auto& main = item.addons.front();
                    const auto  type = static_cast<std::uint32_t>(item.settings.type);
                    const auto  edid = "FRArmor_" + M::EditorIdPart(job.stem, 28) + "_" + label;

                    EspWriter::Arma arma;
                    arma.id              = nextId + 1;
                    arma.edid            = edid + "AA";
                    arma.slots           = item.slots;
                    arma.armorType       = type;
                    arma.race            = main.race.Empty() ? job.defaultRace : main.race;
                    arma.additionalRaces = main.additionalRaces;
                    // Every addon's races, so a merged torso fits whoever any
                    // of its pieces fitted.
                    for (std::size_t i = 1; i < item.addons.size(); ++i) {
                        for (const auto& r : item.addons[i].additionalRaces) {
                            bool have = false;
                            for (const auto& h : arma.additionalRaces) {
                                have = have || (h.file == r.file && h.local == r.local);
                            }
                            if (!have) {
                                arma.additionalRaces.push_back(r);
                            }
                        }
                    }
                    arma.priority[0] = arma.priority[1] = main.priority;
                    arma.weightSlider[0] = arma.weightSlider[1] = anyPair ? 2 : 0;
                    arma.detectionSoundValue = main.detection;
                    arma.weaponAdjust        = main.weaponAdjust;
                    // One mesh for both sexes: it is the mesh you were
                    // wearing, and an empty slot would make the item invisible
                    // on the other sex.
                    arma.model[0] = arma.model[1] = modelPath;
                    arma.model1st[0] = arma.model1st[1] = first;

                    EspWriter::Armo armo;
                    armo.id            = nextId;
                    armo.edid          = edid;
                    armo.name          = ToCp1252(item.settings.name.empty()
                                                              ? std::string(M::BucketNoun(item.bucket, item.settings.type))
                                                              : item.settings.name);
                    armo.worldModel[0] = armo.worldModel[1] = M::RecordModelPath(item.worldModel);
                    if (item.worldModel.empty()) {
                        armo.worldModel[0] = armo.worldModel[1] = modelPath;
                    }
                    armo.slots     = item.slots;
                    armo.armorType = type;
                    armo.race      = job.defaultRace;
                    armo.keywords  = item.keywords;
                    armo.addons    = { arma.id };
                    armo.value     = std::max(0, item.settings.value);
                    armo.weight    = std::max(0.0f, item.settings.weight);
                    armo.rating    = item.settings.type == ArmorType::kClothing
                                         ? 0.0f
                                         : std::max(0.0f, item.settings.rating);
                    spec.armos.push_back(armo);
                    spec.armas.push_back(arma);

                    if (job.recipe && !job.forge.Empty()) {
                        EspWriter::Cobj cobj;
                        cobj.id        = nextId + 2;
                        cobj.edid      = edid + "Recipe";
                        cobj.workbench = job.forge;
                        cobj.created   = armo.id;
                        spec.cobjs.push_back(cobj);
                    }
                    nextId += 3;
                    ++written;
                }
                if (written == 0) {
                    Fail(job, "No mesh could be written." +
                                      (job.notes.empty() ? std::string{} : " " + job.notes.front()));
                    return;
                }

                SetStatus("Writing the plugin...");
                std::string err;
                const auto  espName = job.pluginName + ".esp";
                if (!WriteFile(fs::path("Data") / espName, EspWriter::Serialize(spec), err)) {
                    Fail(job, err);
                    return;
                }

                {
                    std::error_code ec;
                    fs::remove(fs::path("Data\\meshes\\FittingRoom\\Armor") / job.stem / kInProgressFile, ec);
                }
                std::string msg = "Made " + std::to_string(written) + " item" + (written == 1 ? "" : "s") +
                                  " in \"" + espName + "\". Enable it in your mod manager's plugin list and "
                                  "restart the game, then craft them at any forge.";
                if (zeroCtx) {
                    std::size_t              zeroed = 0, notBodySlide = 0;
                    std::vector<std::string> failed;
                    for (const auto& [key, z] : zeroCtx->cache) {
                        if (z.ok) {
                            ++zeroed;
                            continue;
                        }
                        spdlog::info("ArmorExport: exported as built: {} - {}", key, z.why);
                        if (z.why == "not built by BodySlide") {
                            ++notBodySlide;
                        } else {
                            failed.push_back(key + ": " + z.why);
                        }
                    }
                    msg += " Zeroed sliders: " + std::to_string(zeroed) + " mesh" + (zeroed == 1 ? "" : "es") +
                           " rebuilt from BodySlide";
                    if (notBodySlide > 0) {
                        msg += ", " + std::to_string(notBodySlide) + " not built by BodySlide and left as they are";
                    }
                    if (!failed.empty()) {
                        msg += ", " + std::to_string(failed.size()) + " left as built because " + failed.front() +
                               (failed.size() > 1 ? " (and more, see the log)" : "");
                    }
                    msg += ".";
                }
                if (!job.notes.empty()) {
                    msg += " Note: " + job.notes.front();
                    if (job.notes.size() > 1) {
                        msg += " (+" + std::to_string(job.notes.size() - 1) + " more in the log)";
                    }
                    for (const auto& note : job.notes) {
                        spdlog::info("ArmorExport: note - {}", note);
                    }
                }
                Finish(true, msg);
            } catch (const std::exception& e) {
                Fail(job, std::string("Export failed: ") + e.what());
            } catch (...) {
                Fail(job, "Export failed with an unknown error.");
            }
        }

        // ---- Readback: render thread -----------------------------------------

        bool ReadBack(ID3D11Device* a_dev, ID3D11DeviceContext* a_ctx, Bake& a_b) {
            auto* rd = a_b.texture ? reinterpret_cast<OS::RendererData*>(a_b.texture->rendererTexture) : nullptr;
            if (!rd || !rd->texture) {
                return false;
            }
            D3D11_TEXTURE2D_DESC d{};
            rd->texture->GetDesc(&d);
            const bool rgba8 = d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                               d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            if (!rgba8 || d.ArraySize != 1 || d.SampleDesc.Count != 1) {
                return false;  // not one of the dye's own 2D RGBA8 textures
            }
            a_b.srgb = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            if (rd->resourceView) {
                D3D11_SHADER_RESOURCE_VIEW_DESC v{};
                rd->resourceView->GetDesc(&v);
                a_b.srgb = v.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            }
            D3D11_TEXTURE2D_DESC sd = d;
            sd.Usage          = D3D11_USAGE_STAGING;
            sd.BindFlags      = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags      = 0;
            ComPtr<ID3D11Texture2D> staging;
            if (FAILED(a_dev->CreateTexture2D(&sd, nullptr, &staging)) || !staging) {
                return false;
            }
            a_ctx->CopyResource(staging.Get(), rd->texture);
            a_b.width  = d.Width;
            a_b.height = d.Height;
            a_b.mips.clear();
            for (UINT level = 0; level < d.MipLevels; ++level) {
                D3D11_MAPPED_SUBRESOURCE m{};
                if (FAILED(a_ctx->Map(staging.Get(), level, D3D11_MAP_READ, 0, &m)) || !m.pData) {
                    return false;
                }
                const UINT w   = std::max(1u, d.Width >> level);
                const UINT h   = std::max(1u, d.Height >> level);
                const auto row = static_cast<std::size_t>(w) * 4u;
                std::vector<std::uint8_t> px(row * h);
                const auto* src = static_cast<const std::uint8_t*>(m.pData);
                for (UINT y = 0; y < h; ++y) {
                    std::memcpy(px.data() + y * row, src + static_cast<std::size_t>(y) * m.RowPitch, row);
                }
                a_ctx->Unmap(staging.Get(), level);
                a_b.mips.push_back(std::move(px));
            }
            return !a_b.mips.empty();
        }
    }

    Plan PlanFor(const Outfit& a_outfit, std::string_view a_outfitName) {
        Plan                                          plan;
        std::array<ItemPlan, M::kBucketCount>         items{};
        std::array<bool, M::kBucketCount>             typed{};
        std::vector<StyleRefKey>                      seen;
        auto*      player   = RE::PlayerCharacter::GetSingleton();
        auto*      base     = player ? player->GetActorBase() : nullptr;
        const auto sex      = base && base->GetSex() == RE::SEX::kFemale ? RE::SEX::kFemale : RE::SEX::kMale;
        const auto otherSex = sex == RE::SEX::kFemale ? RE::SEX::kMale : RE::SEX::kFemale;
        auto*      race     = player ? player->GetRace() : nullptr;
        a_outfit.ForEachStyle([&](std::uint32_t, const StyleRefKey& a_key) {
            for (const auto& s : seen) {
                if (s == a_key) {
                    return;
                }
            }
            seen.push_back(a_key);
            auto* armo = StyleRef::Resolve(a_key);
            if (!armo) {
                plan.skipped.push_back("a piece from " + a_key.modName + " (not in your load order)");
                return;
            }
            const char*        n     = armo->GetName();
            const std::string  label = (n && *n) ? n : "(unnamed)";
            const auto         slots = armo->GetSlotMask().underlying();
            const auto         b     = M::BucketFor(slots);
            if (b == Bucket::kSkip) {
                plan.skipped.push_back(label + " (shields are not merged into the torso)");
                return;
            }
            auto& it = items[static_cast<std::size_t>(b)];
            it.bucket = b;
            it.styles.push_back(a_key);
            it.pieceNames.push_back(label);
            {
                // Where BodySlide put each model's morphs, for the menu's list.
                // Only the armatures that fit your race, when any do: a
                // beast-race mesh's morphs are not on what you wear.
                bool anyFits = false;
                for (auto* aa : armo->armorAddons) {
                    anyFits = anyFits || (aa && race && aa->IsValidRace(race));
                }
                std::vector<std::string> tris;
                for (auto* aa : armo->armorAddons) {
                    if (!aa || (anyFits && !aa->IsValidRace(race))) {
                        continue;
                    }
                    std::string model = ModelOf(aa->bipedModels[sex]);
                    if (model.empty()) {
                        model = ModelOf(aa->bipedModels[otherSex]);
                    }
                    auto tri = BodyMorphTri::TriPathFor(M::RecordModelPath(model));
                    if (!tri.empty() && std::find(tris.begin(), tris.end(), tri) == tris.end()) {
                        tris.push_back(std::move(tri));
                    }
                }
                it.triPaths.push_back(std::move(tris));
            }
            it.slots |= slots;
            it.defaults.rating += static_cast<float>(armo->armorRating) / 100.0f;
            it.defaults.weight += armo->weight;
            it.defaults.value += static_cast<int>(armo->value);
            // The type comes from the piece that gives the item its name: the
            // body piece for the torso, else the first one seen.
            const bool defining = !typed[static_cast<std::size_t>(b)] ||
                                  (b == Bucket::kTorso && (slots & M::kBodyBit));
            if (defining) {
                typed[static_cast<std::size_t>(b)] = true;
                switch (armo->bipedModelData.armorType.get()) {
                case RE::BGSBipedObjectForm::ArmorType::kHeavyArmor: it.defaults.type = ArmorType::kHeavy; break;
                case RE::BGSBipedObjectForm::ArmorType::kClothing:   it.defaults.type = ArmorType::kClothing; break;
                case RE::BGSBipedObjectForm::ArmorType::kLightArmor:
                default:                                             it.defaults.type = ArmorType::kLight; break;
                }
            }
        });
        const std::string outfit = a_outfitName.empty() ? std::string("Outfit") : std::string(a_outfitName);
        for (auto b : { Bucket::kTorso, Bucket::kHelmet, Bucket::kGloves, Bucket::kBoots }) {
            auto& it = items[static_cast<std::size_t>(b)];
            if (it.styles.empty()) {
                continue;
            }
            it.defaults.name = outfit + " " + M::BucketNoun(b, it.defaults.type);
            plan.items.push_back(std::move(it));
        }
        return plan;
    }

    std::vector<std::string> MorphNames(const std::vector<std::string>& a_triPaths) {
        std::set<std::string> names;
        for (const auto& rel : a_triPaths) {
            std::error_code ec;
            const auto      path = fs::path("Data") / "meshes" / rel;
            if (!fs::is_regular_file(path, ec) || ec) {
                continue;
            }
            const auto size = fs::file_size(path, ec);
            if (ec || size == 0 || size > 256ull * 1024ull * 1024ull) {
                continue;
            }
            std::ifstream in(path, std::ios::binary);
            std::string   text(static_cast<std::size_t>(size), '\0');
            if (!in.read(text.data(), static_cast<std::streamsize>(size))) {
                continue;
            }
            for (auto& n : BodyMorphTri::ParseNames(text)) {
                names.insert(std::move(n));
            }
        }
        return { names.begin(), names.end() };
    }

    bool Start(Request a_request) {
        {
            std::scoped_lock l(g_statusLock);
            if (g_status.busy) {
                return false;
            }
            g_status          = Status{};
            g_status.busy     = true;
            g_status.message  = "Reading your character...";
        }
        auto* task = SKSE::GetTaskInterface();
        if (!task) {
            Finish(false, "SKSE task interface unavailable.");
            return false;
        }
        task->AddTask([req = std::move(a_request)]() mutable {
            try {
                Gather(std::move(req));
            } catch (const std::exception& e) {
                Finish(false, std::string("Export failed: ") + e.what());
            } catch (...) {
                Finish(false, "Export failed with an unknown error.");
            }
        });
        return true;
    }

    Status GetStatus() {
        std::scoped_lock l(g_statusLock);
        return g_status;
    }

    void Acknowledge() {
        std::scoped_lock l(g_statusLock);
        if (!g_status.busy) {
            g_status = Status{};
        }
    }

    void PumpRenderThread() {
        std::unique_lock l(g_rbLock);
        if (!g_rbWanted || g_rbDone) {
            return;
        }
        // The worker is blocked in RequestReadback holding nothing but its
        // wait, so the bake it named is stable until we signal.
        Bake& b  = *g_rbWanted;
        bool  ok = false;
        try {
            auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
            auto* dev = renderer ? reinterpret_cast<ID3D11Device*>(renderer->GetRuntimeData().forwarder) : nullptr;
            auto* ctx = renderer ? reinterpret_cast<ID3D11DeviceContext*>(renderer->GetRuntimeData().context)
                                 : nullptr;
            ok = dev && ctx && ReadBack(dev, ctx, b);
        } catch (...) {
            // Out of memory on a huge texture, most likely. That texture stays
            // undyed; the export goes on.
            b.mips.clear();
            ok = false;
        }
        if (!ok) {
            spdlog::warn("ArmorExport: readback failed for {}.", b.file);
        }
        g_rbOk     = ok;
        g_rbDone   = true;
        g_rbWanted = nullptr;
        l.unlock();
        g_rbCv.notify_all();
    }

}  // namespace OS::ArmorExport
