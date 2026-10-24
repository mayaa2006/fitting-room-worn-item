// Tests for "Make real armor" at zeroed sliders (addon): the zero-build offsets
// and the body-slider file rebuilt from a BodySlide project.
#include "ZeroedModel.h"

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
    using namespace OS;
    BodyMorphSource::Source src;
    src.referenceMesh = "CalienteTools\\BodySlide\\ShapeData\\Dress\\Dress.nif";

    auto set = [&](const char* a_key, std::vector<std::uint16_t> a_idx, std::vector<std::array<float, 3>> a_d) {
        BodyMorphData::DeltaSet d;
        d.indices = std::move(a_idx);
        d.deltas  = std::move(a_d);
        src.deltas.emplace(a_key, std::move(d));
    };
    set("dress.osd|dressbreasts", { 0, 2 }, { { 1.0f, 0.0f, 0.0f }, { 0.0f, 2.0f, 0.0f } });
    set("dress.osd|dresssmall", { 1, 9 }, { { 0.0f, 0.0f, -3.0f }, { 5.0f, 5.0f, 5.0f } });  // 9: out of range
    set("dress.osd|dresszap", { 0 }, { { 1.0f, 1.0f, 1.0f } });
    set("dress.osd|dressuv", { 3 }, { { 0.5f, -0.25f, 0.0f } });

    BodyMorphSource::ShapeRules dress{ "DressTarget", {}, "Dress" };
    BodyMorphData::SliderRule breasts;
    breasts.dataName = "dress.osd|dressbreasts";
    breasts.small = breasts.big = 50.0f;  // .osp defaults must NOT leak into the zero build
    BodyMorphData::SliderRule small;
    small.dataName = "dress.osd|dresssmall";
    small.invert   = true;
    BodyMorphData::SliderRule zap;
    zap.dataName = "dress.osd|dresszap";
    zap.zap      = true;
    zap.invert   = true;
    BodyMorphData::SliderRule uv;
    uv.dataName = "dress.osd|dressuv";
    uv.uv       = true;
    dress.rules.emplace("Breasts", breasts);
    dress.rules.emplace("BreastsSmall", small);
    dress.rules.emplace("ZapSkirt", zap);
    dress.rules.emplace("UVFix", uv);
    src.shapes.push_back(dress);
    BodyMorphSource::ShapeRules missing{ "Cape", {}, "" };
    missing.rules.emplace("Breasts", breasts);
    src.shapes.push_back(missing);

    const std::map<std::string, std::size_t> counts{ { "dress", 4 } };  // case differs from the .osp

    {  // Zero build: only the inverted slider moves, at full strength.
        const auto off = ZeroedModel::ZeroOffsets(src, counts);
        CHECK(off.size() == 1 && off.count("dress"));
        if (off.count("dress")) {
            const auto& f = off.at("dress");
            CHECK(f.size() == 4);
            CHECK(f[0][0] == 0.0f);   // Breasts is 0, its .osp default ignored
            CHECK(f[1][2] == -3.0f);  // BreastsSmall inverted: at 0 it is fully on
            CHECK(f[2][1] == 0.0f);
            CHECK(f[0][1] == 0.0f);   // the zap never moves anything
        }
        const auto zaps = ZeroedModel::ZapsOnAtZero(src);
        CHECK(zaps.size() == 1 && zaps[0] == "ZapSkirt");
    }
    {  // The body-slider file, as BodySlide's build writes it.
        const auto tri = ZeroedModel::TriFromSource(src, counts);
        CHECK(tri.size() == 1);
        if (tri.size() == 1) {
            CHECK(tri[0].name == "dress");
            CHECK(tri[0].morphs.size() == 2);  // Breasts, BreastsSmall; zap left out
            CHECK(tri[0].uvMorphs.size() == 1 && tri[0].uvMorphs[0].name == "UVFix");
            if (tri[0].morphs.size() == 2) {
                CHECK(tri[0].morphs[0].name == "Breasts");
                const auto& s = tri[0].morphs[1];
                CHECK(s.name == "BreastsSmall");
                CHECK(s.deltas.size() == 1);  // index 9 is outside the mesh
                // NOT inverted in the file: RaceMenu adds +value x delta.
                CHECK(std::fabs(s.Offset(s.deltas[0])[2] + 3.0f) < 1e-3f);
            }
            const auto back = TriFile::Parse(TriFile::Serialize(tri));
            CHECK(back && (*back)[0].uvMorphs.size() == 1);
        }
        CHECK(ZeroedModel::TriFromSource(src, {}).empty());
    }

    if (g_failures == 0) {
        std::printf("all ZeroedModel tests passed\n");
        return 0;
    }
    std::printf("%d ZeroedModel check(s) failed\n", g_failures);
    return 1;
}
