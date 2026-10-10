#include "PieceMorphs.h"

#include "PieceMorphModel.h"
#include "RaceMenuMorphApi.h"
#include "StyleRef.h"
#include "TriFile.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

namespace OS::PieceMorphs {

    namespace {
        namespace M  = PieceMorphModel;
        namespace fs = std::filesystem;
        using Clock  = std::chrono::steady_clock;

        const fs::path kSettingsFile{ "Data/SKSE/Plugins/FittingRoom/PieceMorphs.json" };

        // ---- settings (any thread) -------------------------------------------

        std::mutex  g_lock;
        bool        g_loaded{ false };
        M::Settings g_settings;
        std::uint64_t g_version{ 1 };  // bumped on every change
        bool          g_dirty{ false };
        Clock::time_point g_changedAt{};
        std::map<std::string, State> g_states;

        void LoadLocked() {
            if (g_loaded) {
                return;
            }
            g_loaded = true;
            std::ifstream in(kSettingsFile, std::ios::binary);
            if (!in) {
                return;
            }
            Json::Value             root;
            Json::CharReaderBuilder b;
            std::string             errs;
            if (!Json::parseFromStream(b, in, &root, &errs)) {
                spdlog::warn("PieceMorphs: {} is not valid JSON ({}); starting empty.", kSettingsFile.string(), errs);
                return;
            }
            g_settings = M::Decode(root);
            spdlog::info("PieceMorphs: {} piece(s) with their own body morphs.", g_settings.size());
        }

        void SaveLocked() {
            std::error_code ec;
            fs::create_directories(kSettingsFile.parent_path(), ec);
            Json::StreamWriterBuilder w;
            w["indentation"]  = "  ";
            const auto text   = Json::writeString(w, M::Encode(g_settings));
            const auto tmp    = fs::path(kSettingsFile.string() + ".tmp");
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out || !out.write(text.data(), static_cast<std::streamsize>(text.size()))) {
                    spdlog::warn("PieceMorphs: could not write {}.", tmp.string());
                    return;
                }
            }
            fs::rename(tmp, kSettingsFile, ec);
            if (ec) {
                fs::remove(tmp, ec);
                spdlog::warn("PieceMorphs: could not replace {}.", kSettingsFile.string());
            }
        }

        // ---- tri copies (worker threads) -------------------------------------

        enum class GenState { kPending, kReady, kFailed };
        struct Gen {
            GenState    state{ GenState::kPending };
            std::string path;  // meshes-relative, as written into BODYTRI
        };
        std::mutex                           g_genLock;
        std::unordered_map<std::string, Gen> g_gens;  // normalized original + prefix -> copy

        std::vector<std::uint8_t> ResourceBytes(const std::string& a_path) {
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

        bool WriteIfChanged(const fs::path& a_path, const std::vector<std::uint8_t>& a_bytes) {
            std::error_code ec;
            if (fs::is_regular_file(a_path, ec) && fs::file_size(a_path, ec) == a_bytes.size()) {
                std::ifstream in(a_path, std::ios::binary);
                std::vector<std::uint8_t> have(a_bytes.size());
                if (in.read(reinterpret_cast<char*>(have.data()), static_cast<std::streamsize>(have.size())) &&
                    have == a_bytes) {
                    return true;  // same copy as last session
                }
            }
            fs::create_directories(a_path.parent_path(), ec);
            const auto tmp = fs::path(a_path.string() + ".tmp");
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out || !out.write(reinterpret_cast<const char*>(a_bytes.data()),
                                       static_cast<std::streamsize>(a_bytes.size()))) {
                    return false;
                }
            }
            fs::rename(tmp, a_path, ec);
            if (ec) {
                fs::remove(tmp, ec);
                return false;
            }
            return true;
        }

        void Generate(std::string a_id, std::string a_original, std::string a_prefix, std::string a_out) {
            bool ok = false;
            try {
                std::string rel = a_original;
                for (auto& c : rel) {
                    c = c == '/' ? '\\' : c;
                }
                if (rel.size() < 7 || _strnicmp(rel.c_str(), "meshes\\", 7) != 0) {
                    rel = "meshes\\" + rel;
                }
                const auto bytes = ResourceBytes(rel);
                if (bytes.empty()) {
                    spdlog::warn("PieceMorphs: could not read {}.", rel);
                } else if (auto tri = TriFile::Parse(bytes); !tri) {
                    spdlog::warn("PieceMorphs: {} is not a body-slider file Fitting Room can read.", rel);
                } else {
                    const auto copy = TriFile::Serialize(M::WithPieceMorphs(*tri, a_prefix));
                    ok = WriteIfChanged(fs::path("Data\\meshes\\" + a_out), copy);
                    if (ok) {
                        spdlog::info("PieceMorphs: {} -> meshes\\{} ({} shape(s), prefix {}).", rel, a_out,
                                     tri->size(), a_prefix);
                    } else {
                        spdlog::warn("PieceMorphs: could not write meshes\\{}.", a_out);
                    }
                }
            } catch (const std::exception& e) {
                spdlog::warn("PieceMorphs: copying {} failed: {}", a_original, e.what());
            }
            std::scoped_lock l(g_genLock);
            auto& g = g_gens[a_id];
            g.state = ok ? GenState::kReady : GenState::kFailed;
        }

        // The copy for this original + prefix: its state, starting the job the
        // first time it is asked for.
        Gen Request(const std::string& a_original, const std::string& a_prefix) {
            const auto out = M::GeneratedPath(a_original, a_prefix);
            std::string id = out;  // the path already hashes original + prefix
            std::scoped_lock l(g_genLock);
            if (const auto it = g_gens.find(id); it != g_gens.end()) {
                return it->second;
            }
            Gen g;
            g.path = out;
            g_gens.emplace(id, g);
            std::thread(Generate, id, a_original, a_prefix, out).detach();
            return g;
        }

        // ---- the heartbeat (main thread) -------------------------------------

        struct PieceInfo {
            StyleRefKey key;
            std::string keyString;
            std::string prefix;
            std::map<std::string, float> morphs;
        };

        Clock::time_point                     g_nextTick{};
        std::uint64_t                         g_builtVersion{ 0 };
        std::unordered_map<RE::TESObjectARMA*, std::size_t> g_byAddon;  // -> index in g_pieces
        std::vector<PieceInfo>                g_pieces;
        std::map<std::string, float>          g_applied;  // what our key holds now
        std::set<std::string>                 g_toldOnce;
        Clock::time_point                     g_nextReassert{};

        void Rebuild(const M::Settings& a_settings) {
            g_pieces.clear();
            g_byAddon.clear();
            for (const auto& [keyString, piece] : a_settings) {
                PieceInfo info;
                if (!M::ParseKey(keyString, info.key.modName, info.key.localFormID)) {
                    continue;
                }
                auto* armo = StyleRef::Resolve(info.key);
                if (!armo) {
                    continue;
                }
                info.keyString = keyString;
                info.prefix    = M::Prefix(keyString);
                info.morphs    = piece.morphs;
                const auto idx = g_pieces.size();
                g_pieces.push_back(std::move(info));
                for (auto* aa : armo->armorAddons) {
                    if (aa) {
                        g_byAddon.emplace(aa, idx);
                    }
                }
            }
        }

        RE::NiStringExtraData* BodyTriOf(RE::NiAVObject* a_root) {
            if (!a_root) {
                return nullptr;
            }
            static const RE::BSFixedString kName{ "BODYTRI" };
            if (auto* sd = netimmerse_cast<RE::NiStringExtraData*>(a_root->GetExtraData(kName))) {
                return sd;
            }
            // One level down, for a root the engine wrapped.
            if (auto* node = a_root->AsNode()) {
                for (auto& child : node->GetChildren()) {
                    if (child) {
                        if (auto* sd = netimmerse_cast<RE::NiStringExtraData*>(child->GetExtraData(kName))) {
                            return sd;
                        }
                    }
                }
            }
            return nullptr;
        }

        void TellOnce(const std::string& a_what) {
            if (g_toldOnce.insert(a_what).second) {
                spdlog::info("PieceMorphs: {}", a_what);
            }
        }
    }

    std::map<std::string, float> Get(const StyleRefKey& a_piece) {
        std::scoped_lock l(g_lock);
        LoadLocked();
        const auto it = g_settings.find(M::KeyString(a_piece.modName, a_piece.localFormID));
        return it == g_settings.end() ? std::map<std::string, float>{} : it->second.morphs;
    }

    void Set(const StyleRefKey& a_piece, const std::string& a_label, std::map<std::string, float> a_values) {
        std::erase_if(a_values, [](const auto& kv) { return kv.second == 0.0f || kv.first.empty(); });
        std::scoped_lock l(g_lock);
        LoadLocked();
        const auto key = M::KeyString(a_piece.modName, a_piece.localFormID);
        if (a_values.empty()) {
            if (g_settings.erase(key) == 0) {
                return;
            }
        } else {
            auto& p  = g_settings[key];
            p.label  = a_label;
            p.morphs = std::move(a_values);
        }
        ++g_version;
        g_dirty     = true;
        g_changedAt = Clock::now();
        g_nextTick  = {};  // apply on the very next heartbeat
    }

    State StateOf(const StyleRefKey& a_piece) {
        std::scoped_lock l(g_lock);
        const auto it = g_states.find(M::KeyString(a_piece.modName, a_piece.localFormID));
        return it == g_states.end() ? State::kNotWorn : it->second;
    }

    void Tick(RE::Actor* a_player) {
        const auto now = Clock::now();
        if (now < g_nextTick) {
            return;
        }
        g_nextTick = now + std::chrono::milliseconds(250);

        M::Settings settings;
        std::uint64_t version;
        {
            std::scoped_lock l(g_lock);
            LoadLocked();
            if (g_dirty && now - g_changedAt > std::chrono::seconds(1)) {
                SaveLocked();
                g_dirty = false;
            }
            version = g_version;
            if (version != g_builtVersion) {
                settings = g_settings;
            }
        }
        if (version != g_builtVersion) {
            Rebuild(settings);
            g_builtVersion = version;
        }
        if (g_pieces.empty() && g_applied.empty()) {
            return;
        }
        if (!RaceMenuMorphApi::Available() || !a_player || !a_player->Get3D()) {
            return;
        }
        const auto& bipedPtr = a_player->GetBiped(false);
        auto*       biped    = bipedPtr.get();
        if (!biped) {
            return;
        }

        std::map<std::string, State> states;
        for (const auto& p : g_pieces) {
            states[p.keyString] = State::kNotWorn;
        }
        std::map<std::string, float> want;
        bool                         relinked = false;
        std::set<RE::NiAVObject*>    seen;
        for (std::uint32_t bit = 0; bit < 32; ++bit) {
            const auto& obj = biped->objects[bit];
            if (!obj.addon || !obj.partClone) {
                continue;
            }
            const auto it = g_byAddon.find(obj.addon);
            if (it == g_byAddon.end() || !seen.insert(obj.partClone.get()).second) {
                continue;
            }
            const auto& piece = g_pieces[it->second];
            auto&       state = states[piece.keyString];
            auto*       sd    = BodyTriOf(obj.partClone.get());
            if (!sd || !sd->value || !sd->value[0]) {
                state = State::kNoSliders;
                TellOnce(piece.keyString + " is worn but its mesh has no BODYTRI link, so it has no body sliders.");
                continue;
            }
            const std::string current = sd->value;
            if (!M::IsGeneratedPath(current)) {
                const auto gen = Request(current, piece.prefix);
                if (gen.state == GenState::kPending) {
                    state = State::kPreparing;
                    continue;
                }
                if (gen.state == GenState::kFailed) {
                    state = State::kFailed;
                    continue;
                }
                // ⚠ REWRITTEN IN PLACE, NEVER REALLOCATED. The string belongs
                // to the engine's allocator and this clone's own copy of the
                // extra data; writing a shorter path into it needs neither.
                if (gen.path.size() > current.size()) {
                    state = State::kPathTooShort;
                    TellOnce(piece.keyString + ": its tri link '" + current +
                             "' is shorter than the copy's path, so it cannot be relinked.");
                    continue;
                }
                std::memcpy(sd->value, gen.path.c_str(), gen.path.size() + 1);
                relinked = true;
                spdlog::info("PieceMorphs: relinked slot {} ({}) from {} to {}.", bit + 30, piece.keyString,
                             current, gen.path);
            }
            state = State::kLive;
            for (auto& [name, value] : M::PrefixedValues(piece.prefix, piece.morphs)) {
                want.emplace(std::move(name), value);
            }
        }

        // OBody's rebuilds clear keys they do not know (the push-up scar), so
        // a key that should hold values and holds none is put back.
        // At most every two seconds, so a key that will not hold cannot turn
        // into a body rebuild on every heartbeat.
        const bool lost = !g_applied.empty() && now >= g_nextReassert &&
                          !RaceMenuMorphApi::HasPieceMorphs(a_player);
        if (lost) {
            g_nextReassert = now + std::chrono::seconds(2);
        }
        if ((relinked && !want.empty()) || want != g_applied || lost) {
            std::vector<RaceMenuMorphApi::MorphValue> values;
            for (const auto& [name, value] : want) {
                values.push_back({ name, value });
            }
            const auto r = RaceMenuMorphApi::ApplyPieceMorphs(a_player, values);
            if (lost || want.size() != g_applied.size()) {
                spdlog::info("PieceMorphs: applied {} value(s){}; refreshed={}.", r.valuesSet,
                             lost ? " (our key had been cleared)" : "", r.refreshed);
            }
            g_applied = std::move(want);
        }

        std::scoped_lock l(g_lock);
        g_states = std::move(states);
    }

}  // namespace OS::PieceMorphs
