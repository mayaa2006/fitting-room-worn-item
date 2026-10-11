#include "LayerCopies.h"

#include "Collection.h"
#include "EspWriter.h"
#include "LayerCopyModel.h"
#include "PieceMorphModel.h"  // KeyString / ParseKey
#include "StyleRef.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>

namespace OS::LayerCopies {

    namespace {
        namespace L  = LayerCopyModel;
        namespace fs = std::filesystem;

        const fs::path kRegistryFile{ "Data/SKSE/Plugins/FittingRoom/Layers.json" };

        std::mutex    g_lock;
        bool          g_loaded{ false };
        L::Registry   g_reg;

        void LoadLocked() {
            if (g_loaded) {
                return;
            }
            g_loaded = true;
            std::ifstream in(kRegistryFile, std::ios::binary);
            if (!in) {
                return;
            }
            Json::Value             root;
            Json::CharReaderBuilder b;
            std::string             errs;
            if (Json::parseFromStream(b, in, &root, &errs)) {
                g_reg = L::Decode(root);
                spdlog::info("LayerCopies: {} layer cop(ies) on record.", g_reg.entries.size());
            } else {
                spdlog::warn("LayerCopies: {} is not valid JSON ({}).", kRegistryFile.string(), errs);
            }
        }

        bool WriteBytes(const fs::path& a_path, const std::string& a_bytes, std::string& a_err) {
            std::error_code ec;
            fs::create_directories(a_path.parent_path(), ec);
            const auto tmp = fs::path(a_path.string() + ".tmp");
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out || !out.write(a_bytes.data(), static_cast<std::streamsize>(a_bytes.size()))) {
                    a_err = "cannot write " + tmp.string();
                    return false;
                }
            }
            fs::rename(tmp, a_path, ec);
            if (ec) {
                fs::remove(tmp, ec);
                a_err = "cannot replace " + a_path.string() + " (" + ec.message() + ")";
                return false;
            }
            return true;
        }

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

        std::string ModelOf(const RE::TESModel& a_model) {
            const char* m = a_model.GetModel();
            return (m && *m) ? std::string(m) : std::string{};
        }

        std::string ToAnsi(const std::string& a_utf8) {
            if (a_utf8.empty()) {
                return a_utf8;
            }
            const int wlen = MultiByteToWideChar(CP_UTF8, 0, a_utf8.data(), static_cast<int>(a_utf8.size()), nullptr, 0);
            if (wlen <= 0) {
                return a_utf8;
            }
            std::wstring w(static_cast<std::size_t>(wlen), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, a_utf8.data(), static_cast<int>(a_utf8.size()), w.data(), wlen);
            const int len = WideCharToMultiByte(CP_ACP, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
            if (len <= 0) {
                return a_utf8;
            }
            std::string out(static_cast<std::size_t>(len), '\0');
            WideCharToMultiByte(CP_ACP, 0, w.data(), wlen, out.data(), len, nullptr, nullptr);
            return out;
        }

        RE::TESObjectARMO* SourceOf(const L::Entry& a_e) {
            StyleRefKey key;
            if (!PieceMorphModel::ParseKey(a_e.source, key.modName, key.localFormID)) {
                return nullptr;
            }
            return StyleRef::Resolve(key);
        }

        RE::TESObjectARMO* CopyOf(const L::Entry& a_e) {
            auto* dh = RE::TESDataHandler::GetSingleton();
            return dh && a_e.armo ? dh->LookupForm<RE::TESObjectARMO>(a_e.armo, L::kPluginName) : nullptr;
        }

        // Rewrites the plugin from a_reg (IDs may be added to it). Notes say
        // what could not come along.
        bool WritePluginLocked(L::Registry& a_reg, std::vector<std::string>& a_notes, std::string& a_err) {
            EspWriter::PluginSpec spec;
            spec.author      = "Fitting Room";
            spec.description = "Layer copies: pieces moved to a free slot so two can be worn in one.";
            const auto defaultRace = RefOf(RE::TESForm::LookupByID<RE::TESRace>(0x19));

            for (auto& e : a_reg.entries) {
                auto* armo = SourceOf(e);
                if (!armo) {
                    a_notes.push_back("\"" + e.name + "\" is kept on record but left out: its mod is not loaded");
                    continue;
                }
                std::vector<RE::TESObjectARMA*> addons;
                for (auto* aa : armo->armorAddons) {
                    if (aa) {
                        addons.push_back(aa);
                    }
                }
                if (!L::EnsureIds(a_reg, e, addons.size())) {
                    a_err = "the layer plugin is full (about a thousand copies); remove some first";
                    return false;
                }
                const auto slots = L::SlotBit(e.slot);
                const auto type  = static_cast<std::uint32_t>(armo->bipedModelData.armorType.get());
                const char* rawName = armo->GetName();
                const std::string name = rawName && *rawName ? rawName : (e.name.empty() ? "Piece" : e.name);
                for (std::size_t i = 0; i < addons.size(); ++i) {
                    auto*          aa = addons[i];
                    EspWriter::Arma a;
                    a.id        = e.armas[i];
                    a.edid      = L::EditorId(name, e.slot, a.id);
                    a.slots     = slots;
                    a.armorType = type;
                    a.race      = RefOf(aa->race);
                    if (a.race.Empty()) {
                        a.race = defaultRace;
                    }
                    for (auto* r : aa->additionalRaces) {
                        if (auto ref = RefOf(r); !ref.Empty()) {
                            a.additionalRaces.push_back(std::move(ref));
                        }
                    }
                    for (int s = 0; s < 2; ++s) {
                        a.priority[s]     = aa->data.priorities[s];
                        a.weightSlider[s] = aa->data.modelRange[s];
                        a.model[s]        = ModelOf(aa->bipedModels[s]);
                        a.model1st[s]     = ModelOf(aa->bipedModel1stPersons[s]);
                        if (aa->bipedModels[s].numAlternateTextures > 0) {
                            a_notes.push_back("\"" + name +
                                              "\" uses record texture swaps the copy cannot carry, so it shows "
                                              "the mesh's own textures");
                        }
                    }
                    a.detectionSoundValue = aa->data.detectionSoundValue;
                    a.weaponAdjust        = aa->data.weaponAdjust;
                    a.footstep            = RefOf(aa->footstepSet);
                    spec.armas.push_back(std::move(a));
                }
                EspWriter::Armo o;
                o.id   = e.armo;
                o.edid = L::EditorId(name, e.slot, e.armo);
                o.name = ToAnsi(L::CopyName(name, e.slot));
                for (int s = 0; s < 2; ++s) {
                    o.worldModel[s] = ModelOf(armo->worldModels[s]);
                }
                o.slots     = slots;
                o.armorType = type;
                o.race      = RefOf(armo->race);
                if (o.race.Empty()) {
                    o.race = defaultRace;
                }
                for (std::uint32_t k = 0; k < armo->numKeywords; ++k) {
                    if (auto ref = RefOf(armo->keywords[k]); !ref.Empty()) {
                        o.keywords.push_back(std::move(ref));
                    }
                }
                for (std::size_t i = 0; i < addons.size(); ++i) {
                    o.addons.push_back(e.armas[i]);
                }
                o.value  = armo->value;
                o.weight = armo->weight;
                o.rating = static_cast<float>(armo->armorRating) / 100.0f;
                spec.armos.push_back(std::move(o));
                e.name = name;
            }

            // Masters in load order.
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
                for (const auto& a : spec.armas) {
                    note(a.race);
                    for (const auto& r : a.additionalRaces) note(r);
                    note(a.footstep);
                }
                for (const auto& o : spec.armos) {
                    note(o.race);
                    for (const auto& k : o.keywords) note(k);
                }
                std::stable_sort(ranked.begin(), ranked.end(),
                                 [](const auto& x, const auto& y) { return x.first < y.first; });
                for (auto& [rank, n] : ranked) {
                    spec.masterOrder.push_back(std::move(n));
                }
            }

            const auto bytes = EspWriter::Serialize(spec);
            if (!WriteBytes(fs::path("Data") / L::kPluginName, std::string(bytes.begin(), bytes.end()), a_err)) {
                return false;
            }
            Json::StreamWriterBuilder w;
            w["indentation"] = "  ";
            if (!WriteBytes(kRegistryFile, Json::writeString(w, L::Encode(a_reg)), a_err)) {
                return false;
            }
            spdlog::info("LayerCopies: wrote {} with {} cop(ies).", L::kPluginName, spec.armos.size());
            return true;
        }

        std::string NotesText(const std::vector<std::string>& a_notes) {
            std::set<std::string> uniq(a_notes.begin(), a_notes.end());
            std::string           out;
            for (const auto& n : uniq) {
                out += " Note: " + n + ".";
            }
            return out;
        }
    }

    std::vector<Row> List() {
        std::scoped_lock l(g_lock);
        LoadLocked();
        std::vector<Row> out;
        for (const auto& e : g_reg.entries) {
            Row r;
            r.name        = L::CopyName(e.name, e.slot);
            r.slot        = e.slot;
            r.loaded      = CopyOf(e) != nullptr;
            r.sourceFound = SourceOf(e) != nullptr;
            out.push_back(std::move(r));
        }
        return out;
    }

    bool Add(const StyleRefKey& a_source, std::uint32_t a_slot, std::string& a_message) {
        std::scoped_lock l(g_lock);
        LoadLocked();
        if (!L::SlotBit(a_slot) || a_slot < 44) {
            a_message = "Pick a slot from 44 to 61.";
            return false;
        }
        auto* armo = StyleRef::Resolve(a_source);
        if (!armo) {
            a_message = "That piece is not in your load order.";
            return false;
        }
        const auto key = PieceMorphModel::KeyString(a_source.modName, a_source.localFormID);
        if (const auto* have = L::Find(g_reg, key, a_slot)) {
            a_message = "\"" + L::CopyName(have->name, a_slot) + "\" already exists." +
                        (CopyOf(*have) ? " Pick it for slot " + std::to_string(a_slot) + " in your outfit."
                                       : " Enable " + std::string(L::kPluginName) +
                                             " in your plugin list and restart the game to use it.");
            return true;
        }
        auto reg = g_reg;  // only kept if the write works
        L::Entry e;
        e.source = key;
        e.slot   = a_slot;
        const char* n = armo->GetName();
        e.name   = n && *n ? n : "Piece";
        reg.entries.push_back(e);
        std::vector<std::string> notes;
        std::string              err;
        if (!WritePluginLocked(reg, notes, err)) {
            a_message = "Not made: " + err;
            return false;
        }
        g_reg     = std::move(reg);
        a_message = "Made \"" + L::CopyName(e.name, a_slot) + "\" in " + L::kPluginName +
                    ". Enable that plugin in your mod manager (once), restart the game, then pick the copy for slot " +
                    std::to_string(a_slot) + " in your outfit. Shape pieces can give it its own shape." +
                    NotesText(notes);
        return true;
    }

    bool Remove(std::size_t a_index, std::string& a_message) {
        std::scoped_lock l(g_lock);
        LoadLocked();
        if (a_index >= g_reg.entries.size()) {
            return false;
        }
        auto reg = g_reg;
        const auto name = L::CopyName(reg.entries[a_index].name, reg.entries[a_index].slot);
        reg.entries.erase(reg.entries.begin() + static_cast<std::ptrdiff_t>(a_index));
        std::vector<std::string> notes;
        std::string              err;
        if (!WritePluginLocked(reg, notes, err)) {
            a_message = "Not removed: " + err;
            return false;
        }
        g_reg     = std::move(reg);
        a_message = "Removed \"" + name + "\". It is gone from outfits after the next restart.";
        return true;
    }

    void Tick() {
        static auto next = std::chrono::steady_clock::time_point{};
        const auto  now  = std::chrono::steady_clock::now();
        if (now < next) {
            return;
        }
        next = now + std::chrono::seconds(2);
        std::scoped_lock l(g_lock);
        LoadLocked();
        if (g_reg.entries.empty()) {
            return;
        }
        auto& collection = Collection::GetSingleton();
        for (const auto& e : g_reg.entries) {
            auto* copy = CopyOf(e);
            auto* src  = copy ? SourceOf(e) : nullptr;
            if (src && collection.Knows(src->GetFormID()) && !collection.Knows(copy->GetFormID())) {
                collection.Add(copy);
            }
        }
    }

}  // namespace OS::LayerCopies
