// Tests for live per-piece body morphs, engine-free half (addon).
#include "PieceMorphModel.h"

#include <cmath>
#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

int main() {
    using namespace OS::PieceMorphModel;

    {  // Keys round-trip; prefixes are short, stable and distinct.
        const auto k = KeyString("Dress Mod.esp", 0x00012AB);
        CHECK(k == "Dress Mod.esp|000012AB");
        std::string   mod;
        std::uint32_t id = 0;
        CHECK(ParseKey(k, mod, id) && mod == "Dress Mod.esp" && id == 0x12AB);
        CHECK(ParseKey("a|b|0000000f", mod, id) && mod == "a|b" && id == 0xF);
        CHECK(!ParseKey("nobar", mod, id));
        CHECK(!ParseKey("|00000001", mod, id));
        CHECK(!ParseKey("x|123", mod, id));
        CHECK(!ParseKey("x|0000000G", mod, id));
        const auto p = Prefix(k);
        CHECK(p.size() == 10 && p.rfind("FRP", 0) == 0 && p.back() == '_');
        CHECK(Prefix(k) == p);
        CHECK(Prefix(KeyString("Dress Mod.esp", 0x12AC)) != p);
    }
    {  // The copy's path: short, under our folder, per original + prefix.
        const auto g = GeneratedPath("Armor\\Dress\\dress.tri", "FRPaaaaaa_");
        CHECK(g.rfind("FittingRoom\\M\\", 0) == 0 && g.ends_with(".tri"));
        CHECK(g.size() == 26);
        CHECK(GeneratedPath("meshes/armor/DRESS/dress.tri", "FRPaaaaaa_") == g);  // same file
        CHECK(GeneratedPath("Armor\\Dress\\dress.tri", "FRPbbbbbb_") != g);
        CHECK(GeneratedPath("Armor\\Other\\dress.tri", "FRPaaaaaa_") != g);
        CHECK(IsGeneratedPath(g));
        CHECK(IsGeneratedPath("fittingroom/m/12345678.tri"));
        CHECK(IsGeneratedPath("meshes\\FittingRoom\\M\\x.tri"));
        CHECK(!IsGeneratedPath("Armor\\Dress\\dress.tri"));
        CHECK(!IsGeneratedPath("FittingRoom\\Armor\\T\\Torso.tri"));
    }
    {  // The tri copy keeps every morph and adds a prefixed twin of each.
        OS::TriFile::File tri;
        OS::TriFile::Shape s;
        s.name = "Dress";
        OS::TriFile::Morph a;
        a.name       = "Breasts";
        a.multiplier = 0.5f;
        a.deltas     = { { 1, 2, 3, 4 } };
        OS::TriFile::Morph longName;
        longName.name = std::string(250, 'x');  // the twin would pass 255 bytes
        s.morphs = { a, longName };
        tri.push_back(s);
        const auto out = WithPieceMorphs(tri, "FRP123456_");
        CHECK(out.size() == 1);
        CHECK(out[0].morphs.size() == 3);
        CHECK(out[0].morphs[0].name == "Breasts");
        CHECK(out[0].morphs[2].name == "FRP123456_Breasts");
        CHECK(out[0].morphs[2].deltas.size() == 1 && out[0].morphs[2].multiplier == 0.5f);
        // Applying it twice does not twin the twins.
        const auto again = WithPieceMorphs(out, "FRP123456_");
        CHECK(again[0].morphs.size() == 3);
        // And it serializes and reads back.
        const auto back = OS::TriFile::Parse(OS::TriFile::Serialize(out));
        CHECK(back && (*back)[0].morphs.size() == 3);
    }
    {  // Values for the key: prefixed, zeros and junk dropped.
        const auto v = PrefixedValues("FRP1_", { { "Breasts", 0.5f }, { "Butt", 0.0f }, { "", 1.0f } });
        CHECK(v.size() == 1 && v.count("FRP1_Breasts") && v.at("FRP1_Breasts") == 0.5f);
    }
    {  // Settings file round trip, lenient decode.
        Settings s;
        s["Dress.esp|00000801"] = Piece{ "Red Dress", { { "Breasts", 0.3f }, { "Hips", -0.25f } } };
        s["Empty.esp|00000001"] = Piece{ "Nothing", { { "Breasts", 0.0f } } };
        const auto back = Decode(Encode(s));
        CHECK(back.size() == 1);
        CHECK(back.count("Dress.esp|00000801"));
        if (back.count("Dress.esp|00000801")) {
            const auto& p = back.at("Dress.esp|00000801");
            CHECK(p.label == "Red Dress");
            CHECK(p.morphs.size() == 2);
            CHECK(std::fabs(p.morphs.at("Hips") + 0.25f) < 1e-6f);
        }
        Json::Value junk;
        junk["pieces"]["a|00000001"]["morphs"]["Breasts"] = "lots";
        junk["pieces"]["b|00000001"]["morphs"]["Breasts"] = 9.0;
        junk["pieces"]["c|00000001"]                      = 5;
        const auto j = Decode(junk);
        CHECK(j.size() == 1 && j.count("b|00000001") && j.at("b|00000001").morphs.at("Breasts") == 2.0f);
        CHECK(Decode(Json::Value("x")).empty());
    }

    if (g_failures == 0) {
        std::printf("all PieceMorphModel tests passed\n");
        return 0;
    }
    std::printf("%d PieceMorphModel check(s) failed\n", g_failures);
    return 1;
}
