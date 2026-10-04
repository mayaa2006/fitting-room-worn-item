#include "ArmorExport.h"

#include "EspWriter.h"
#include "NifExport.h"
#include "RendererData.h"
#include "StyleRef.h"

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
#include <set>
#include <thread>

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

        constexpr const char* kMarkerFile = "FittingRoomExport.txt";

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
                         RE::SEX a_sex) {
            Addon ad;
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
            job->stem       = M::SafeStem(job->pluginName, 24, "Outfit");
            job->recipe     = a_req.recipe;

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
                // A finished export always has its .esp and a marker file in
                // its mesh folder; folders WITHOUT the marker are what a failed
                // or interrupted export left behind, and are cleared.
                std::error_code ec;
                const auto meshDir = fs::path("Data\\meshes\\FittingRoom\\Armor") / job->stem;
                const auto texDir  = fs::path("Data\\textures\\FittingRoom\\Armor") / job->stem;
                if (fs::exists(fs::path("Data") / (job->pluginName + ".esp"), ec)) {
                    Finish(false, "\"" + job->pluginName + ".esp\" already exists. Pick a new plugin name.");
                    return;
                }
                if (fs::exists(meshDir / kMarkerFile, ec)) {
                    Finish(false, "An earlier export already uses the folder \"" + job->stem +
                                      "\". Pick a more different plugin name.");
                    return;
                }
                fs::remove_all(meshDir, ec);
                fs::remove_all(texDir, ec);
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
                for (const auto& key : plan.styles) {
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
                        GatherAddon(*job, item, obj.addon, obj, sex);
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

        // One NIF for an item: every addon's model rewritten, then merged.
        // a_variant is "_0", "_1" or "" (single file); a_firstPerson picks the
        // first-person models instead. Returns false with a_err on failure, or
        // true with empty a_out when no addon has such a model.
        bool BuildNif(const Item& a_item, const std::string& a_variant, bool a_firstPerson,
                      std::vector<std::uint8_t>& a_out, std::vector<std::string>& a_notes, std::string& a_err) {
            std::vector<std::vector<std::uint8_t>> parts;
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
                parts.push_back(std::move(r.bytes));
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
            if (merged.droppedExtra > 0 && !a_firstPerson && a_variant != "_0") {
                a_notes.push_back(std::to_string(merged.droppedExtra) + " piece(s) merged into the " +
                                  M::BucketLabel(a_item.bucket) +
                                  " had body-slider or physics data that one merged mesh cannot keep; "
                                  "they will not follow body sliders");
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
                    SetCopying(true);
                    b.read = RequestReadback(b);
                    SetCopying(false);
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
                    for (const auto& v : variants) {
                        std::vector<std::uint8_t> bytes;
                        if (!BuildNif(item, v, false, bytes, job.notes, err)) {
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
                        ++wroteVariants;
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
                            if (!BuildNif(item, v, true, bytes, job.notes, err)) {
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
                    std::ofstream marker(fs::path("Data\\meshes\\FittingRoom\\Armor") / job.stem / kMarkerFile);
                    marker << espName << "\n";
                }
                std::string msg = "Made " + std::to_string(written) + " item" + (written == 1 ? "" : "s") +
                                  " in \"" + espName + "\". Enable it in your mod manager's plugin list and "
                                  "restart the game, then craft them at any forge.";
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
