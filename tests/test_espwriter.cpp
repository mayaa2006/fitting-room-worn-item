// Pure-logic tests for the "Make real armor" plugin writer (addon). Parses the
// bytes back with an independent little reader and checks every header size,
// subrecord order and form id the game will read.
#include "EspWriter.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

namespace {
    using Bytes = std::vector<std::uint8_t>;

    std::uint32_t U32(const Bytes& b, std::size_t at) {
        return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
               (static_cast<std::uint32_t>(b[at + 2]) << 16) |
               (static_cast<std::uint32_t>(b[at + 3]) << 24);
    }
    std::uint16_t U16(const Bytes& b, std::size_t at) {
        return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
    }
    float F32(const Bytes& b, std::size_t at) {
        const std::uint32_t v = U32(b, at);
        float               f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    std::string Tag(const Bytes& b, std::size_t at) {
        return std::string(reinterpret_cast<const char*>(b.data() + at), 4);
    }

    struct SubRec {
        std::string tag;
        Bytes       data;
        [[nodiscard]] std::string Str() const {
            return data.empty() ? std::string{}
                                : std::string(reinterpret_cast<const char*>(data.data()),
                                              data.size() - 1);  // drop the NUL
        }
    };
    struct Rec {
        std::string         tag;
        std::uint32_t       flags{ 0 };
        std::uint32_t       formID{ 0 };
        std::uint16_t       formVersion{ 0 };
        std::vector<SubRec> subs;
        [[nodiscard]] const SubRec* Find(const char* t, int nth = 0) const {
            for (const auto& s : subs) {
                if (s.tag == t && nth-- == 0) {
                    return &s;
                }
            }
            return nullptr;
        }
        [[nodiscard]] std::vector<std::string> Tags() const {
            std::vector<std::string> r;
            for (const auto& s : subs) {
                r.push_back(s.tag);
            }
            return r;
        }
    };

    // Reads one record at `at`; returns false on any size inconsistency.
    bool ReadRecord(const Bytes& b, std::size_t& at, Rec& out) {
        if (at + 24 > b.size()) {
            return false;
        }
        out.tag         = Tag(b, at);
        const auto size = U32(b, at + 4);
        out.flags       = U32(b, at + 8);
        out.formID      = U32(b, at + 12);
        out.formVersion = U16(b, at + 20);
        std::size_t p   = at + 24;
        const auto  end = p + size;
        if (end > b.size()) {
            return false;
        }
        while (p < end) {
            if (p + 6 > end) {
                return false;
            }
            SubRec s;
            s.tag         = Tag(b, p);
            const auto sz = U16(b, p + 4);
            p += 6;
            if (p + sz > end) {
                return false;
            }
            s.data.assign(b.begin() + static_cast<std::ptrdiff_t>(p),
                          b.begin() + static_cast<std::ptrdiff_t>(p + sz));
            p += sz;
            out.subs.push_back(std::move(s));
        }
        at = end;
        return true;
    }
}

int main() {
    using namespace OS::EspWriter;

    PluginSpec spec;
    spec.author      = "Fitting Room";
    spec.description = "test";
    spec.masterOrder = { "Skyrim.esm", "Update.esm", "Dawnguard.esm", "SomeArmor.esp" };

    const FormRef defaultRace{ "Skyrim.esm", 0x19 };
    const FormRef nord{ "SKYRIM.ESM", 0x13746 };            // case differs: same master
    const FormRef dlcRace{ "Dawnguard.esm", 0x283A };
    const FormRef lightKw{ "SomeArmor.esp", 0x801 };
    const FormRef forge{ "Skyrim.esm", 0x88105 };

    Arma arma;
    arma.id              = 0x801;
    arma.edid            = "FRArmor_TestAA";
    arma.slots           = 0x4;  // body
    arma.armorType       = 0;
    arma.race            = defaultRace;
    arma.additionalRaces = { nord, dlcRace };
    arma.priority[0] = 1;
    arma.priority[1] = 2;
    arma.weightSlider[0] = 2;
    arma.weightSlider[1] = 2;
    arma.detectionSoundValue = 3;
    arma.weaponAdjust        = 1.5f;
    arma.model[0]  = "FittingRoom\\Armor\\Test\\a_1.nif";
    arma.model[1]  = "FittingRoom\\Armor\\Test\\a_1.nif";
    arma.model1st[0] = "FittingRoom\\Armor\\Test\\a_1st.nif";
    spec.armas.push_back(arma);

    Armo armo;
    armo.id        = 0x800;
    armo.edid      = "FRArmor_Test";
    armo.name      = "Test Cuirass";
    armo.slots     = 0x4;
    armo.armorType = 1;
    armo.race      = defaultRace;
    armo.keywords  = { FormRef{ "Skyrim.esm", 0x6BBD2 }, lightKw };
    armo.addons    = { 0x801 };
    armo.value     = 125;
    armo.weight    = 7.5f;
    armo.rating    = 25.5f;
    armo.worldModel[0] = "Armor\\Iron\\IronCuirassGND.nif";
    spec.armos.push_back(armo);

    Cobj cobj;
    cobj.id        = 0x802;
    cobj.edid      = "FRArmor_TestRecipe";
    cobj.workbench = forge;
    cobj.created   = 0x800;
    spec.cobjs.push_back(cobj);

    const auto bytes = Serialize(spec);
    CHECK(!bytes.empty());

    std::size_t at = 0;
    Rec         tes4;
    CHECK(ReadRecord(bytes, at, tes4));
    CHECK(tes4.tag == "TES4");
    CHECK(tes4.flags == 0x200);  // ESL flag, not localized, not a master
    CHECK(tes4.formID == 0);
    CHECK(tes4.formVersion == 44);

    // HEDR: version, 3 records + 3 groups, next id past the highest.
    const auto* hedr = tes4.Find("HEDR");
    CHECK(hedr && hedr->data.size() == 12);
    if (hedr && hedr->data.size() == 12) {
        CHECK(F32(hedr->data, 0) > 1.69f && F32(hedr->data, 0) < 1.71f);
        CHECK(U32(hedr->data, 4) == 6);
        CHECK(U32(hedr->data, 8) == 0x803);
    }
    CHECK(tes4.Find("CNAM") && tes4.Find("CNAM")->Str() == "Fitting Room");
    CHECK(tes4.Find("SNAM") && tes4.Find("SNAM")->Str() == "test");

    // Masters: only the ones referenced, deduplicated case-insensitively, in
    // load order (Update.esm is listed but unreferenced, so absent).
    std::vector<std::string> mast;
    for (const auto& s : tes4.subs) {
        if (s.tag == "MAST") {
            mast.push_back(s.Str());
        }
    }
    CHECK(mast.size() == 3);
    if (mast.size() == 3) {
        CHECK(mast[0] == "Skyrim.esm");
        CHECK(mast[1] == "Dawnguard.esm");
        CHECK(mast[2] == "SomeArmor.esp");
    }
    // Every MAST is followed by an 8-byte DATA.
    for (std::size_t i = 0; i + 1 < tes4.subs.size(); ++i) {
        if (tes4.subs[i].tag == "MAST") {
            CHECK(tes4.subs[i + 1].tag == "DATA" && tes4.subs[i + 1].data.size() == 8);
        }
    }
    const std::uint32_t own = 3u << 24;  // three masters

    // Groups in canonical order: ARMO, COBJ, ARMA - each sized to its end.
    const char* expectGroups[] = { "ARMO", "COBJ", "ARMA" };
    std::vector<Rec> recs;
    for (const char* g : expectGroups) {
        CHECK(at + 24 <= bytes.size());
        if (at + 24 > bytes.size()) {
            break;
        }
        CHECK(Tag(bytes, at) == "GRUP");
        CHECK(Tag(bytes, at + 8) == g);
        CHECK(U32(bytes, at + 12) == 0);
        const auto gsize = U32(bytes, at + 4);
        const auto gend  = at + gsize;
        CHECK(gend <= bytes.size());
        at += 24;
        while (at < gend) {
            Rec r;
            const bool ok = ReadRecord(bytes, at, r);
            CHECK(ok);
            if (!ok) {
                break;
            }
            CHECK(r.tag == g);
            recs.push_back(std::move(r));
        }
        CHECK(at == gend);
    }
    CHECK(at == bytes.size());
    CHECK(recs.size() == 3);
    if (recs.size() != 3) {
        std::printf("%d EspWriter check(s) failed\n", g_failures);
        return 1;
    }

    {  // ARMO
        const auto& r = recs[0];
        CHECK(r.formID == (own | 0x800));
        CHECK(r.formVersion == 44);
        const std::vector<std::string> order = { "EDID", "OBND", "FULL", "MOD2", "BOD2", "RNAM",
                                                 "KSIZ", "KWDA", "DESC", "MODL", "DATA", "DNAM" };
        CHECK(r.Tags() == order);
        CHECK(r.Find("EDID")->Str() == "FRArmor_Test");
        CHECK(r.Find("FULL")->Str() == "Test Cuirass");
        CHECK(r.Find("OBND")->data.size() == 12);
        CHECK(U32(r.Find("BOD2")->data, 0) == 0x4 && U32(r.Find("BOD2")->data, 4) == 1);
        CHECK(U32(r.Find("RNAM")->data, 0) == 0x00000019);
        CHECK(U32(r.Find("KSIZ")->data, 0) == 2);
        CHECK(r.Find("KWDA")->data.size() == 8);
        CHECK(U32(r.Find("KWDA")->data, 0) == 0x0006BBD2);
        CHECK(U32(r.Find("KWDA")->data, 4) == ((2u << 24) | 0x801));  // ESL master, index 2
        CHECK(U32(r.Find("MODL")->data, 0) == (own | 0x801));
        CHECK(U32(r.Find("DATA")->data, 0) == 125);
        CHECK(F32(r.Find("DATA")->data, 4) == 7.5f);
        CHECK(U32(r.Find("DNAM")->data, 0) == 2550);  // 25.5 x 100
        CHECK(r.Find("DESC")->data.size() == 1);
    }
    {  // COBJ
        const auto& r = recs[1];
        CHECK(r.formID == (own | 0x802));
        const std::vector<std::string> order = { "EDID", "CNAM", "BNAM", "NAM1" };
        CHECK(r.Tags() == order);
        CHECK(U32(r.Find("CNAM")->data, 0) == (own | 0x800));
        CHECK(U32(r.Find("BNAM")->data, 0) == 0x00088105);
        CHECK(r.Find("NAM1")->data.size() == 2 && U16(r.Find("NAM1")->data, 0) == 1);
    }
    {  // ARMA
        const auto& r = recs[2];
        CHECK(r.formID == (own | 0x801));
        const std::vector<std::string> order = { "EDID", "BOD2", "RNAM", "DNAM", "MOD2",
                                                 "MOD3", "MOD4", "MODL", "MODL" };
        CHECK(r.Tags() == order);
        const auto& d = r.Find("DNAM")->data;
        CHECK(d.size() == 12);
        if (d.size() == 12) {
            CHECK(d[0] == 1 && d[1] == 2 && d[2] == 2 && d[3] == 2 && d[6] == 3);
            CHECK(F32(d, 8) == 1.5f);
        }
        CHECK(r.Find("MOD2")->Str() == "FittingRoom\\Armor\\Test\\a_1.nif");
        CHECK(r.Find("MOD4")->Str() == "FittingRoom\\Armor\\Test\\a_1st.nif");
        CHECK(U32(r.Find("MODL", 0)->data, 0) == 0x00013746);
        CHECK(U32(r.Find("MODL", 1)->data, 0) == ((1u << 24) | 0x283A));
    }

    {  // A plugin with no masters at all still numbers its own records index 0,
       // and an ESP without the light flag says so.
        PluginSpec bare;
        bare.light = false;
        Armo a;
        a.id   = 0x800;
        a.edid = "X";
        bare.armos.push_back(a);
        const auto b2 = Serialize(bare);
        std::size_t p = 0;
        Rec         h;
        CHECK(ReadRecord(b2, p, h));
        CHECK(h.flags == 0);
        CHECK(!h.Find("MAST"));
        p += 24;  // the ARMO group header
        Rec r;
        CHECK(ReadRecord(b2, p, r));
        CHECK(r.formID == 0x800);
    }

    if (g_failures == 0) {
        std::printf("all EspWriter tests passed\n");
        return 0;
    }
    std::printf("%d EspWriter check(s) failed\n", g_failures);
    return 1;
}
