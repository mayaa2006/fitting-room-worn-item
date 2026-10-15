#pragma once

// Hidden parts (addon): the co-save side record 'FHID'.
//
// The hidden flag lives on DyeChannel (Outfit.h), so the editor's staging,
// undo, Apply and the outfit JSON carry it like any dye edit. The upstream
// 'LIBR' co-save format is left exactly as it is (v25), so a save never
// becomes unreadable to an upstream build; this record carries the flags
// beside it and is laid back over the library after 'LIBR' and 'NPCO' load.
//
// Per library: the outfits that hide anything, by index AND name (a renamed
// or reordered library still finds them by name), and per outfit the
// (slot, garment, channel mask) triples.
//
// Tested in tests/test_hiddenparts.cpp.

#include "Outfit.h"

#include <json/json.h>

#include <cstdint>
#include <string>

namespace OS::HiddenPartsCodec {

    [[nodiscard]] inline Json::Value EncodeLibrary(const OutfitLibrary& a_lib) {
        Json::Value list(Json::arrayValue);
        for (std::size_t i = 0; i < a_lib.Count(); ++i) {
            const auto* o = a_lib.At(i);
            if (!o) {
                continue;
            }
            Json::Value entries(Json::arrayValue);
            o->ForEachArmourDye([&](std::uint32_t a_bit, const StyleRefKey& a_garment, const SlotDye& a_dye) {
                const auto mask = a_dye.HiddenMask();
                if (mask == 0) {
                    return;
                }
                Json::Value e(Json::objectValue);
                e["b"] = a_bit;
                e["m"] = a_garment.modName;
                e["f"] = a_garment.localFormID;
                e["h"] = mask;
                entries.append(e);
            });
            if (entries.empty()) {
                continue;
            }
            Json::Value j(Json::objectValue);
            j["i"] = static_cast<Json::UInt>(i);
            j["n"] = o->name;
            j["d"] = entries;
            list.append(j);
        }
        return list;
    }

    // Lays a list written by EncodeLibrary over a_lib. An outfit is found at
    // its saved index when the name there still matches, else by name when
    // exactly one outfit has it; otherwise it is skipped. Returns how many
    // (slot, garment) entries were applied.
    inline std::size_t ApplyLibrary(const Json::Value& a_list, OutfitLibrary& a_lib) {
        if (!a_list.isArray()) {
            return 0;
        }
        std::size_t applied = 0;
        for (const auto& j : a_list) {
            if (!j.isObject() || !j["n"].isString() || !j["d"].isArray()) {
                continue;
            }
            const auto name = j["n"].asString();
            Outfit*    target = nullptr;
            if (j["i"].isUInt()) {
                if (auto* o = a_lib.At(j["i"].asUInt()); o && o->name == name) {
                    target = o;
                }
            }
            if (!target) {
                int hits = 0;
                for (auto& o : a_lib.All()) {
                    if (o.name == name) {
                        target = &o;
                        ++hits;
                    }
                }
                if (hits != 1) {
                    target = nullptr;
                }
            }
            if (!target) {
                continue;
            }
            for (const auto& e : j["d"]) {
                if (!e.isObject() || !e["b"].isUInt() || !e["m"].isString() || !e["f"].isUInt() ||
                    !e["h"].isUInt()) {
                    continue;
                }
                const auto bit = e["b"].asUInt();
                if (bit >= kBitCount) {
                    continue;
                }
                StyleRefKey garment;
                garment.modName     = e["m"].asString();
                garment.localFormID = e["f"].asUInt();
                target->SetArmourHiddenMask(bit, garment, e["h"].asUInt());
                ++applied;
            }
        }
        return applied;
    }

}  // namespace OS::HiddenPartsCodec
