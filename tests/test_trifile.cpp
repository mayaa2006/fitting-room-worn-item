// Tests for the body-slider morph file reader/writer (addon, "Make real armor").
#include "TriFile.h"

#include <cmath>
#include <cstdio>
#include <string>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

int main() {
    using namespace OS::TriFile;

    File f;
    {
        Shape body;
        body.name = "BaseShape";
        Morph breasts;
        breasts.name       = "Breasts";
        breasts.multiplier = 0.001f;
        breasts.deltas     = { { 0, 1000, -2000, 32767 }, { 7, -32768, 0, 5 } };
        Morph butt;
        butt.name       = "Butt";
        butt.multiplier = 0.5f;
        butt.deltas     = { { 3, 1, 2, 3 } };
        body.morphs = { breasts, butt };
        Shape skirt;
        skirt.name   = "Skirt";
        Morph empty;
        empty.name       = "Thighs";
        empty.multiplier = 0.0f;
        skirt.morphs     = { empty };
        f = { body, skirt };
    }

    {  // Round trip is exact.
        const auto bytes = Serialize(f);
        CHECK(bytes.size() > 6);
        CHECK(std::string(bytes.begin(), bytes.begin() + 4) == "PIRT");
        const auto back = Parse(bytes);
        CHECK(back.has_value());
        if (back) {
            CHECK(back->size() == 2);
            CHECK((*back)[0].name == "BaseShape");
            CHECK((*back)[0].morphs.size() == 2);
            const auto& m = (*back)[0].morphs[0];
            CHECK(m.name == "Breasts");
            CHECK(m.multiplier == 0.001f);
            CHECK(m.deltas.size() == 2);
            CHECK(m.deltas[0].index == 0 && m.deltas[0].x == 1000 && m.deltas[0].y == -2000 &&
                  m.deltas[0].z == 32767);
            CHECK(m.deltas[1].index == 7 && m.deltas[1].x == -32768 && m.deltas[1].z == 5);
            const auto off = m.Offset(m.deltas[0]);
            CHECK(std::fabs(off[0] - 1.0f) < 1e-5f && std::fabs(off[1] + 2.0f) < 1e-5f);
            CHECK((*back)[1].name == "Skirt" && (*back)[1].morphs[0].deltas.empty());
            CHECK(Serialize(*back) == bytes);
        }
    }

    {  // Not a tri, or cut short anywhere: nothing.
        CHECK(!Parse(std::string_view("")).has_value());
        CHECK(!Parse(std::string_view("PIRX\x01\x00", 6)).has_value());
        const auto bytes = Serialize(f);
        for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
            const std::string_view part(reinterpret_cast<const char*>(bytes.data()), cut);
            CHECK(!Parse(part).has_value());
        }
        // A huge delta count with no data behind it is refused, not reserved.
        std::vector<std::uint8_t> lie{ 'P', 'I', 'R', 'T', 1, 0, 1, 'A', 1, 0, 1, 'M', 0, 0, 0, 0, 0xFF, 0xFF };
        CHECK(!Parse(lie).has_value());
        // Trailing bytes after a complete file are tolerated.
        auto longer = Serialize(f);
        longer.push_back(0);
        CHECK(Parse(longer).has_value());
        // An empty file of zero shapes is valid.
        const auto none = Parse(Serialize(File{}));
        CHECK(none.has_value() && none->empty());
    }

    {  // Renamed: merged names, case-blind fallback, unmatched shapes dropped.
        std::map<std::string, std::string> names{ { "BaseShape", "BaseShape" }, { "skirt", "Skirt_2" } };
        const auto r = Renamed(f, names);
        CHECK(r.size() == 2);
        CHECK(r.size() == 2 && r[0].name == "BaseShape" && r[1].name == "Skirt_2");
        CHECK(r.size() == 2 && r[0].morphs.size() == 2);
        // Exact match beats the case-blind one.
        std::map<std::string, std::string> both{ { "Skirt", "A" }, { "skirt", "B" } };
        const auto e = Renamed(f, both);
        CHECK(e.size() == 1 && e[0].name == "A");
        // Two case-blind candidates and no exact one: ambiguous, dropped.
        std::map<std::string, std::string> amb{ { "SKIRT", "A" }, { "skirT", "B" } };
        CHECK(Renamed(f, amb).empty());
        CHECK(Renamed(f, {}).empty());
    }

    if (g_failures == 0) {
        std::printf("all TriFile tests passed\n");
        return 0;
    }
    std::printf("%d TriFile check(s) failed\n", g_failures);
    return 1;
}
