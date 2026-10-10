#pragma once

// Live per-piece body morphs (addon): the engine-free half.
//
// RaceMenu keeps one value per morph NAME per character and applies it to
// every worn mesh whose tri file has that name, so it cannot shape one piece
// on its own. The trick: give the piece a copy of its tri in which every
// morph also exists under a name only that piece uses ("FRP1a2b3c_Breasts"
// beside "Breasts"), point the piece's live BODYTRI link at the copy, and set
// the prefixed names under Fitting Room's own RaceMenu key. RaceMenu sums
// morphs, so the piece follows the body (the plain names) and its own extra
// shape (the prefixed ones), and nothing else moves.
//
// Settings are per piece of armor (the ARMO), not per outfit: "this dress
// always gets a bit more room" holds wherever the dress is worn.
//
// Tested in tests/test_piecemorphmodel.cpp.

#include "ArmorExportModel.h"  // HashHex
#include "TriFile.h"

#include <json/json.h>

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace OS::PieceMorphModel {

    // A piece's identity in the settings file: its defining plugin and local
    // form ID, the same pair StyleRefKey holds.
    [[nodiscard]] inline std::string KeyString(std::string_view a_modName, std::uint32_t a_localFormID) {
        char hex[16];
        std::snprintf(hex, sizeof(hex), "%08X", a_localFormID);
        return std::string(a_modName) + "|" + hex;
    }

    // KeyString read back. False for anything it did not write.
    [[nodiscard]] inline bool ParseKey(std::string_view a_key, std::string& a_modName, std::uint32_t& a_id) {
        const auto bar = a_key.rfind('|');
        if (bar == std::string_view::npos || bar == 0 || a_key.size() - bar - 1 != 8) {
            return false;
        }
        std::uint32_t id = 0;
        for (char c : a_key.substr(bar + 1)) {
            int d;
            if (c >= '0' && c <= '9') {
                d = c - '0';
            } else if (c >= 'A' && c <= 'F') {
                d = c - 'A' + 10;
            } else if (c >= 'a' && c <= 'f') {
                d = c - 'a' + 10;
            } else {
                return false;
            }
            id = (id << 4) | static_cast<std::uint32_t>(d);
        }
        a_modName = std::string(a_key.substr(0, bar));
        a_id      = id;
        return true;
    }

    // The piece's own morph-name prefix. Short (morph names are capped at 255
    // bytes) and stable across sessions, since RaceMenu saves values by name.
    [[nodiscard]] inline std::string Prefix(std::string_view a_key) {
        return "FRP" + ArmorExportModel::HashHex(a_key).substr(0, 6) + "_";
    }

    inline constexpr std::string_view kGeneratedDir = "FittingRoom\\M\\";

    namespace detail {
        inline std::string Normal(std::string_view a_path) {
            std::string s;
            for (char c : a_path) {
                s.push_back(c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            while (!s.empty() && s.front() == '\\') {
                s.erase(s.begin());
            }
            if (s.rfind("meshes\\", 0) == 0) {
                s.erase(0, 7);
            }
            return s;
        }
    }

    // Where the piece's tri copy goes, meshes-relative. Kept SHORT on purpose:
    // the live link is rewritten in place inside the engine's own string, so
    // the new path must fit where the old one was (see PieceMorphs.cpp).
    [[nodiscard]] inline std::string GeneratedPath(std::string_view a_originalTri, std::string_view a_prefix) {
        const auto h = ArmorExportModel::HashHex(detail::Normal(a_originalTri) + "|" + std::string(a_prefix));
        return std::string(kGeneratedDir) + h.substr(0, 8) + ".tri";
    }

    [[nodiscard]] inline bool IsGeneratedPath(std::string_view a_path) {
        return detail::Normal(a_path).rfind(detail::Normal(kGeneratedDir), 0) == 0;
    }

    // The tri copy: every shape keeps its morphs and gains a prefixed twin of
    // each. Twins that would overflow the format (a 255-byte name, 65535
    // morphs on a shape) are left out rather than written wrong.
    [[nodiscard]] inline TriFile::File WithPieceMorphs(const TriFile::File& a_tri, std::string_view a_prefix) {
        TriFile::File out = a_tri;
        for (auto& shape : out) {
            const auto original = shape.morphs.size();
            std::set<std::string> have;
            for (const auto& m : shape.morphs) {
                have.insert(m.name);
            }
            for (std::size_t i = 0; i < original; ++i) {
                if (shape.morphs.size() >= 0xFFFF) {
                    break;
                }
                const auto& m = shape.morphs[i];
                if (m.name.empty() || m.name.size() + a_prefix.size() > 255 ||
                    m.name.rfind(a_prefix, 0) == 0) {
                    continue;
                }
                auto twin = m;
                twin.name = std::string(a_prefix) + m.name;
                if (!have.insert(twin.name).second) {
                    continue;  // already twinned
                }
                shape.morphs.push_back(std::move(twin));
            }
        }
        return out;
    }

    // One piece's settings.
    struct Piece {
        std::string                  label;   // the piece's name, for people reading the file
        std::map<std::string, float> morphs;  // plain morph name -> value (1 = 100%)
    };

    // Piece key -> settings. A piece with no non-zero value is not kept.
    using Settings = std::map<std::string, Piece>;

    // The values to put under Fitting Room's RaceMenu key for one piece.
    [[nodiscard]] inline std::map<std::string, float> PrefixedValues(std::string_view a_prefix,
                                                                     const std::map<std::string, float>& a_morphs) {
        std::map<std::string, float> out;
        for (const auto& [name, value] : a_morphs) {
            if (value != 0.0f && std::isfinite(value) && !name.empty() && name.size() + a_prefix.size() <= 255) {
                out.emplace(std::string(a_prefix) + name, value);
            }
        }
        return out;
    }

    [[nodiscard]] inline Json::Value Encode(const Settings& a_settings) {
        Json::Value root(Json::objectValue);
        root["version"] = 1;
        Json::Value pieces(Json::objectValue);
        for (const auto& [key, piece] : a_settings) {
            Json::Value morphs(Json::objectValue);
            for (const auto& [name, value] : piece.morphs) {
                if (value != 0.0f && std::isfinite(value)) {
                    morphs[name] = static_cast<double>(value);
                }
            }
            if (morphs.empty()) {
                continue;
            }
            Json::Value p(Json::objectValue);
            p["label"]  = piece.label;
            p["morphs"] = morphs;
            pieces[key] = p;
        }
        root["pieces"] = pieces;
        return root;
    }

    // Lenient: anything unreadable is skipped, never fatal, so a hand edit
    // that breaks one entry costs that entry. Values are clamped to the
    // menu's range.
    [[nodiscard]] inline Settings Decode(const Json::Value& a_root) {
        Settings out;
        if (!a_root.isObject() || !a_root["pieces"].isObject()) {
            return out;
        }
        const auto& pieces = a_root["pieces"];
        for (const auto& key : pieces.getMemberNames()) {
            const auto& p = pieces[key];
            if (key.empty() || !p.isObject() || !p["morphs"].isObject()) {
                continue;
            }
            Piece piece;
            if (p["label"].isString()) {
                piece.label = p["label"].asString();
            }
            const auto& morphs = p["morphs"];
            for (const auto& name : morphs.getMemberNames()) {
                if (name.empty() || !morphs[name].isNumeric()) {
                    continue;
                }
                auto v = static_cast<float>(morphs[name].asDouble());
                if (!std::isfinite(v) || v == 0.0f) {
                    continue;
                }
                v = v < -2.0f ? -2.0f : (v > 2.0f ? 2.0f : v);
                piece.morphs.emplace(name, v);
            }
            if (!piece.morphs.empty()) {
                out.emplace(key, std::move(piece));
            }
        }
        return out;
    }

}  // namespace OS::PieceMorphModel
