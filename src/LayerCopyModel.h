#pragma once

// Layer copies (addon): the engine-free half.
//
// Skyrim holds one armor addon per biped slot, so two pieces that both use
// slot 32 cannot be worn together - the later one replaces the earlier. The
// usual modder fix is a copy of one piece that uses a free slot (56, 58...).
// This keeps the list of such copies (Data/SKSE/Plugins/FittingRoom/
// Layers.json) and the rules for naming them and giving them stable form IDs
// in "FittingRoom Layers.esp", which LayerCopies.cpp writes from it.
//
// Tested in tests/test_layercopymodel.cpp.

#include "ArmorExportModel.h"  // EditorIdPart

#include <json/json.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace OS::LayerCopyModel {

    inline constexpr const char* kPluginName = "FittingRoom Layers.esp";

    // Own object IDs: ESL range.
    inline constexpr std::uint32_t kFirstId = 0x800;
    inline constexpr std::uint32_t kLastId  = 0xFFF;

    struct SlotInfo {
        std::uint32_t slot;  // 44..61
        const char*   name;
    };

    // The slots a layer can move to, in the order they are suggested: the
    // ones mods use least for real gear first. 52 is late because SOS uses it.
    inline constexpr SlotInfo kLayerSlots[] = {
        { 56, "chest secondary" }, { 58, "arm secondary" },  { 54, "leg" },
        { 57, "shoulder" },        { 60, "misc" },           { 48, "misc / ring" },
        { 53, "leg secondary" },   { 59, "arm" },            { 49, "pelvis secondary" },
        { 55, "face alternate" },  { 45, "neck / scarf" },   { 47, "backpack" },
        { 46, "cloak" },           { 44, "face / mouth" },   { 52, "pelvis / underwear" },
        { 61, "FX" },
    };

    [[nodiscard]] inline std::uint32_t SlotBit(std::uint32_t a_slot) {
        return a_slot >= 30 && a_slot <= 61 ? (1u << (a_slot - 30)) : 0u;
    }

    // The first suggested slot no piece of the outfit uses (a_usedMask is a
    // biped bitmask, bit 0 = slot 30), or 0 when all are taken.
    [[nodiscard]] inline std::uint32_t SuggestSlot(std::uint32_t a_usedMask) {
        for (const auto& s : kLayerSlots) {
            if (!(a_usedMask & SlotBit(s.slot))) {
                return s.slot;
            }
        }
        return 0;
    }

    [[nodiscard]] inline const char* SlotName(std::uint32_t a_slot) {
        for (const auto& s : kLayerSlots) {
            if (s.slot == a_slot) {
                return s.name;
            }
        }
        return "";
    }

    struct Entry {
        std::string                source;  // PieceMorphModel::KeyString form: "Mod.esp|0000ABCD"
        std::uint32_t              slot{ 0 };
        std::string                name;    // the source's name when it was made
        std::uint32_t              armo{ 0 };
        std::vector<std::uint32_t> armas;   // grown as the source gains addons
    };

    struct Registry {
        std::vector<Entry> entries;
        std::uint32_t      nextId{ kFirstId };  // never reused, even after a removal
    };

    [[nodiscard]] inline const Entry* Find(const Registry& a_reg, std::string_view a_source, std::uint32_t a_slot) {
        for (const auto& e : a_reg.entries) {
            if (e.source == a_source && e.slot == a_slot) {
                return &e;
            }
        }
        return nullptr;
    }

    // The next free object ID, or 0 when the plugin is full.
    [[nodiscard]] inline std::uint32_t Allocate(Registry& a_reg) {
        if (a_reg.nextId < kFirstId) {
            a_reg.nextId = kFirstId;
        }
        if (a_reg.nextId > kLastId) {
            return 0;
        }
        return a_reg.nextId++;
    }

    // Makes sure the entry has an ID for its ARMO and for each of a_addonCount
    // addons. False when the plugin ran out of IDs (nothing half-assigned is
    // left behind in that case).
    [[nodiscard]] inline bool EnsureIds(Registry& a_reg, Entry& a_entry, std::size_t a_addonCount) {
        const std::size_t need = (a_entry.armo ? 0 : 1) +
                                 (a_addonCount > a_entry.armas.size() ? a_addonCount - a_entry.armas.size() : 0);
        if (a_reg.nextId < kFirstId) {
            a_reg.nextId = kFirstId;
        }
        if (a_reg.nextId + need > kLastId + 1) {
            return false;
        }
        if (!a_entry.armo) {
            a_entry.armo = Allocate(a_reg);
        }
        while (a_entry.armas.size() < a_addonCount) {
            a_entry.armas.push_back(Allocate(a_reg));
        }
        return true;
    }

    [[nodiscard]] inline std::string CopyName(std::string_view a_name, std::uint32_t a_slot) {
        return std::string(a_name.empty() ? std::string_view("Piece") : a_name) + " [slot " +
               std::to_string(a_slot) + "]";
    }

    [[nodiscard]] inline std::string EditorId(std::string_view a_name, std::uint32_t a_slot, std::uint32_t a_id) {
        char hex[8];
        std::snprintf(hex, sizeof(hex), "%03X", a_id & 0xFFF);
        return "FRLayer_" + ArmorExportModel::EditorIdPart(a_name, 24) + "_" + std::to_string(a_slot) + "_" + hex;
    }

    [[nodiscard]] inline Json::Value Encode(const Registry& a_reg) {
        Json::Value root(Json::objectValue);
        root["version"] = 1;
        root["nextId"]  = a_reg.nextId;
        Json::Value list(Json::arrayValue);
        for (const auto& e : a_reg.entries) {
            Json::Value j(Json::objectValue);
            j["source"] = e.source;
            j["slot"]   = e.slot;
            j["name"]   = e.name;
            j["armo"]   = e.armo;
            Json::Value armas(Json::arrayValue);
            for (auto id : e.armas) {
                armas.append(id);
            }
            j["armas"] = armas;
            list.append(j);
        }
        root["copies"] = list;
        return root;
    }

    // Lenient per entry; IDs outside the ESL range or reused are dropped with
    // their entry, and nextId always ends above every ID kept.
    [[nodiscard]] inline Registry Decode(const Json::Value& a_root) {
        Registry reg;
        if (!a_root.isObject()) {
            return reg;
        }
        if (a_root["nextId"].isUInt()) {
            reg.nextId = a_root["nextId"].asUInt();
        }
        std::vector<std::uint32_t> used;
        auto okId = [&](std::uint32_t id) {
            if (id < kFirstId || id > kLastId) {
                return false;
            }
            for (auto u : used) {
                if (u == id) {
                    return false;
                }
            }
            return true;
        };
        const auto& list = a_root["copies"];
        if (list.isArray()) {
            for (const auto& j : list) {
                if (!j.isObject() || !j["source"].isString() || !j["slot"].isUInt() || !j["armo"].isUInt()) {
                    continue;
                }
                Entry e;
                e.source = j["source"].asString();
                e.slot   = j["slot"].asUInt();
                e.name   = j["name"].isString() ? j["name"].asString() : std::string{};
                e.armo   = j["armo"].asUInt();
                if (e.source.empty() || !SlotBit(e.slot) || e.slot < 44 || !okId(e.armo)) {
                    continue;
                }
                std::vector<std::uint32_t> mine{ e.armo };
                bool                       bad = false;
                if (j["armas"].isArray()) {
                    for (const auto& a : j["armas"]) {
                        if (!a.isUInt()) {
                            bad = true;
                            break;
                        }
                        const auto id = a.asUInt();
                        bool       dup = false;
                        for (auto m : mine) {
                            dup = dup || m == id;
                        }
                        if (dup || !okId(id)) {
                            bad = true;
                            break;
                        }
                        mine.push_back(id);
                        e.armas.push_back(id);
                    }
                }
                if (bad || Find(reg, e.source, e.slot)) {
                    continue;
                }
                used.insert(used.end(), mine.begin(), mine.end());
                reg.entries.push_back(std::move(e));
            }
        }
        for (auto id : used) {
            if (reg.nextId <= id) {
                reg.nextId = id + 1;
            }
        }
        if (reg.nextId < kFirstId) {
            reg.nextId = kFirstId;
        }
        return reg;
    }

}  // namespace OS::LayerCopyModel
