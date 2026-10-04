#pragma once

// "Make real armor" export (addon): pure helpers shared by the engine half
// (ArmorExport.cpp), the NIF rewriter (NifExport.cpp) and the tests. No engine
// types and no nifly here.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace OS::ArmorExportModel {

    enum class ArmorType : std::uint8_t { kLight = 0, kHeavy = 1, kClothing = 2 };

    [[nodiscard]] inline const char* ArmorTypeLabel(ArmorType a_t) {
        switch (a_t) {
        case ArmorType::kLight:    return "Light";
        case ArmorType::kHeavy:    return "Heavy";
        case ArmorType::kClothing: return "Clothing";
        }
        return "Light";
    }

    namespace detail {
        [[nodiscard]] inline bool StartsWithI(std::string_view s, std::string_view p) {
            if (s.size() < p.size()) {
                return false;
            }
            for (std::size_t i = 0; i < p.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(s[i])) !=
                    std::tolower(static_cast<unsigned char>(p[i]))) {
                    return false;
                }
            }
            return true;
        }
        [[nodiscard]] inline bool EndsWithI(std::string_view s, std::string_view p) {
            return s.size() >= p.size() && StartsWithI(s.substr(s.size() - p.size()), p);
        }
        [[nodiscard]] inline std::string Slashes(std::string_view s) {
            std::string r(s);
            std::replace(r.begin(), r.end(), '/', '\\');
            // Collapse doubled separators a hand-authored path sometimes has.
            std::string out;
            out.reserve(r.size());
            for (char c : r) {
                if (c == '\\' && !out.empty() && out.back() == '\\') {
                    continue;
                }
                out.push_back(c);
            }
            return out;
        }
        [[nodiscard]] inline std::string StripLeading(std::string s) {
            while (!s.empty() && (s.front() == '\\' || s.front() == ' ')) {
                s.erase(s.begin());
            }
            if (StartsWithI(s, ".\\")) {
                s.erase(0, 2);
            }
            if (StartsWithI(s, "data\\")) {
                s.erase(0, 5);
            }
            return s;
        }
    }

    // A texture path as a comparison key: lower case, backslashes, no
    // "data\" and no "textures\" prefix. "Textures/Armor/X.dds" and
    // "armor\x.dds" are the same key.
    [[nodiscard]] inline std::string TextureKey(std::string_view a_path) {
        auto s = detail::StripLeading(detail::Slashes(a_path));
        if (detail::StartsWithI(s, "textures\\")) {
            s.erase(0, 9);
        }
        for (auto& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    }

    // A texture path as written into a NIF: backslashes, "textures\" prefix,
    // original case kept. Empty stays empty.
    [[nodiscard]] inline std::string NifTexturePath(std::string_view a_path) {
        auto s = detail::StripLeading(detail::Slashes(a_path));
        if (s.empty()) {
            return s;
        }
        if (!detail::StartsWithI(s, "textures\\")) {
            s = "textures\\" + s;
        }
        return s;
    }

    // A model path as stored in a record (relative to meshes\) and as read
    // through the archive (with it).
    [[nodiscard]] inline std::string RecordModelPath(std::string_view a_path) {
        auto s = detail::StripLeading(detail::Slashes(a_path));
        if (detail::StartsWithI(s, "meshes\\")) {
            s.erase(0, 7);
        }
        return s;
    }
    [[nodiscard]] inline std::string ArchiveMeshPath(std::string_view a_path) {
        return "meshes\\" + RecordModelPath(a_path);
    }

    // Dye textures are named by DyeTexture with the cache key
    // "<identity>|<colour>|...|<quality>", where identity is the source
    // texture's own name (its path). These pull that apart.
    [[nodiscard]] inline bool IsDyeTextureName(std::string_view a_name) {
        return a_name.find('|') != std::string_view::npos;
    }
    [[nodiscard]] inline std::string DyeIdentity(std::string_view a_name) {
        const auto bar = a_name.find('|');
        return bar == std::string_view::npos ? std::string{} : std::string(a_name.substr(0, bar));
    }
    // A 512 px preview build rather than the full-size commit.
    [[nodiscard]] inline bool IsDyePreviewName(std::string_view a_name) {
        return detail::EndsWithI(a_name, "|p");
    }

    // The weight variants of an armor model. "...\Cuirass_1.nif" and
    // "...\Cuirass_0.nif" are one model in two bodyweights; anything else is a
    // single file.
    struct MeshVariant {
        std::string suffix;  // "_0", "_1", or "" for a single file
        std::string path;    // record-relative path of this variant
    };
    [[nodiscard]] inline std::vector<MeshVariant> MeshVariants(std::string_view a_modelPath) {
        const auto rel = RecordModelPath(a_modelPath);
        if (detail::EndsWithI(rel, "_0.nif") || detail::EndsWithI(rel, "_1.nif")) {
            const auto stem = rel.substr(0, rel.size() - 6);
            return { { "_0", stem + "_0.nif" }, { "_1", stem + "_1.nif" } };
        }
        return { { "", rel } };
    }

    // Letters, digits, space, '-' and '_' only; trimmed and capped. Used for
    // the plugin file name and display. Never empty.
    [[nodiscard]] inline std::string SafeName(std::string_view a_in, std::size_t a_max,
                                              std::string_view a_fallback) {
        std::string out;
        for (char c : a_in) {
            const auto u = static_cast<unsigned char>(c);
            if (std::isalnum(u) || c == ' ' || c == '-' || c == '_') {
                out.push_back(c);
            }
        }
        while (!out.empty() && out.front() == ' ') {
            out.erase(out.begin());
        }
        while (!out.empty() && out.back() == ' ') {
            out.pop_back();
        }
        if (out.size() > a_max) {
            out.resize(a_max);
            while (!out.empty() && out.back() == ' ') {
                out.pop_back();
            }
        }
        return out.empty() ? std::string(a_fallback) : out;
    }
    // As SafeName, with spaces as underscores: folder and file stems.
    [[nodiscard]] inline std::string SafeStem(std::string_view a_in, std::size_t a_max,
                                              std::string_view a_fallback) {
        auto s = SafeName(a_in, a_max, a_fallback);
        std::replace(s.begin(), s.end(), ' ', '_');
        return s;
    }
    // Letters and digits only: editor ids.
    [[nodiscard]] inline std::string EditorIdPart(std::string_view a_in, std::size_t a_max) {
        std::string out;
        for (char c : a_in) {
            if (std::isalnum(static_cast<unsigned char>(c))) {
                out.push_back(c);
            }
            if (out.size() >= a_max) {
                break;
            }
        }
        return out.empty() ? std::string("Piece") : out;
    }

    // Biped slot bits (bit 0 = slot 30).
    inline constexpr std::uint32_t kHeadBits   = (1u << 0) | (1u << 1);  // 30 head, 31 hair
    inline constexpr std::uint32_t kBodyBit    = 1u << 2;                // 32
    inline constexpr std::uint32_t kHandsBit   = 1u << 3;                // 33
    inline constexpr std::uint32_t kAmuletBit  = 1u << 5;                // 35
    inline constexpr std::uint32_t kRingBit    = 1u << 6;                // 36
    inline constexpr std::uint32_t kFeetBit    = 1u << 7;                // 37
    inline constexpr std::uint32_t kShieldBit  = 1u << 9;                // 39
    inline constexpr std::uint32_t kCircletBit = 1u << 12;               // 42

    // The four items an export makes (user 2026-10-03: "only the basic slots -
    // torso, boots, gloves, and helmet. Everything not boots gloves or helmet
    // should be combined into one nif").
    enum class Bucket : std::uint8_t { kTorso = 0, kHelmet = 1, kGloves = 2, kBoots = 3, kSkip = 4 };
    inline constexpr std::size_t kBucketCount = 4;

    [[nodiscard]] inline const char* BucketLabel(Bucket a_b) {
        switch (a_b) {
        case Bucket::kTorso:  return "Torso";
        case Bucket::kHelmet: return "Helmet";
        case Bucket::kGloves: return "Gloves";
        case Bucket::kBoots:  return "Boots";
        case Bucket::kSkip:   return "Skipped";
        }
        return "Torso";
    }
    // Default item noun per bucket, armour or clothing.
    [[nodiscard]] inline const char* BucketNoun(Bucket a_b, ArmorType a_t) {
        const bool c = a_t == ArmorType::kClothing;
        switch (a_b) {
        case Bucket::kTorso:  return c ? "Clothes" : "Cuirass";
        case Bucket::kHelmet: return c ? "Hood" : "Helmet";
        case Bucket::kGloves: return c ? "Gloves" : "Gauntlets";
        case Bucket::kBoots:  return c ? "Shoes" : "Boots";
        case Bucket::kSkip:   return "";
        }
        return "";
    }

    // Which item a styled piece goes into, by the slots it covers. The body
    // slot wins (a robe with a hood is a torso piece), then hands, feet, head.
    // A shield is SKIPPED: its mesh hangs off the shield bone rather than the
    // skeleton's skin, so merged into a torso it would sit at the feet.
    // Everything else - cloaks, belts, pauldrons, jewelry - joins the torso.
    [[nodiscard]] inline Bucket BucketFor(std::uint32_t a_slots) {
        if (a_slots & kBodyBit) {
            return Bucket::kTorso;
        }
        if (a_slots & kHandsBit) {
            return Bucket::kGloves;
        }
        if (a_slots & kFeetBit) {
            return Bucket::kBoots;
        }
        if (a_slots & (kHeadBits | kCircletBit)) {
            return Bucket::kHelmet;
        }
        if (a_slots & kShieldBit) {
            return Bucket::kSkip;
        }
        return Bucket::kTorso;
    }

    // Editor ids of the Skyrim.esm keywords a piece of this type covering
    // these slots should carry, so perks, vendors and the smithing menu treat
    // it like the real thing. Resolved by editor id in game, never hard-coded
    // form ids.
    [[nodiscard]] inline std::vector<std::string> KeywordEditorIds(std::uint32_t a_slots,
                                                                   ArmorType     a_type) {
        std::vector<std::string> k;
        const bool clothing = a_type == ArmorType::kClothing;
        const bool jewelry  = (a_slots & (kAmuletBit | kRingBit)) != 0 &&
                             (a_slots & ~(kAmuletBit | kRingBit | kCircletBit)) == 0;
        if (clothing) {
            k.push_back(jewelry ? "ArmorJewelry" : "ArmorClothing");
        } else {
            k.push_back(a_type == ArmorType::kHeavy ? "ArmorHeavy" : "ArmorLight");
        }
        if (a_slots & kBodyBit) {
            k.push_back(clothing ? "ClothingBody" : "ArmorCuirass");
        }
        if (a_slots & kHandsBit) {
            k.push_back(clothing ? "ClothingHands" : "ArmorGauntlets");
        }
        if (a_slots & kFeetBit) {
            k.push_back(clothing ? "ClothingFeet" : "ArmorBoots");
        }
        if (a_slots & kHeadBits) {
            k.push_back(clothing ? "ClothingHead" : "ArmorHelmet");
        } else if (a_slots & kCircletBit) {
            k.push_back(clothing ? "ClothingCirclet" : "ArmorHelmet");
        }
        if ((a_slots & kShieldBit) && !clothing) {
            k.push_back("ArmorShield");
        }
        if (a_slots & kRingBit) {
            k.push_back("ClothingRing");
        }
        if (a_slots & kAmuletBit) {
            k.push_back("ClothingNecklace");
        }
        k.push_back(jewelry ? "VendorItemJewelry"
                            : (clothing ? "VendorItemClothing" : "VendorItemArmor"));
        return k;
    }

}  // namespace OS::ArmorExportModel
