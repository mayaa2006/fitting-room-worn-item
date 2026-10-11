// Tests for layer copies, engine-free half (addon).
#include "LayerCopyModel.h"

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
    using namespace OS::LayerCopyModel;

    {  // Slots.
        CHECK(SlotBit(30) == 1u && SlotBit(56) == (1u << 26) && SlotBit(61) == (1u << 31));
        CHECK(SlotBit(29) == 0 && SlotBit(62) == 0);
        CHECK(SuggestSlot(0) == 56);
        CHECK(SuggestSlot(SlotBit(56)) == 58);
        CHECK(SuggestSlot(SlotBit(56) | SlotBit(58)) == 54);
        std::uint32_t all = 0;
        for (const auto& s : kLayerSlots) {
            CHECK(s.slot >= 44 && s.slot <= 61);
            all |= SlotBit(s.slot);
        }
        CHECK(SuggestSlot(all) == 0);
        CHECK(std::string(SlotName(46)) == "cloak");
        CHECK(std::string(SlotName(32)).empty());
    }
    {  // Names.
        CHECK(CopyName("Red Dress", 56) == "Red Dress [slot 56]");
        CHECK(CopyName("", 58) == "Piece [slot 58]");
        const auto ed = EditorId("Red Dress!", 56, 0x80A);
        CHECK(ed == "FRLayer_RedDress_56_80A");
    }
    {  // IDs: ARMO + one per addon, never reused, all or nothing at the end.
        Registry reg;
        Entry    e;
        e.source = "Dress.esp|00000801";
        e.slot   = 56;
        CHECK(EnsureIds(reg, e, 2));
        CHECK(e.armo == 0x800 && e.armas.size() == 2 && e.armas[0] == 0x801 && e.armas[1] == 0x802);
        CHECK(reg.nextId == 0x803);
        CHECK(EnsureIds(reg, e, 3));  // the source gained an addon
        CHECK(e.armas.size() == 3 && e.armas[2] == 0x803);
        CHECK(EnsureIds(reg, e, 1));  // fewer is fine, nothing taken
        CHECK(reg.nextId == 0x804);
        Registry full;
        full.nextId = 0xFFE;
        Entry f;
        CHECK(!EnsureIds(full, f, 2));  // needs 3, only 2 left
        CHECK(f.armo == 0 && f.armas.empty() && full.nextId == 0xFFE);
        CHECK(EnsureIds(full, f, 1));
        CHECK(f.armo == 0xFFE && f.armas[0] == 0xFFF);
        CHECK(Allocate(full) == 0);
    }
    {  // Registry round trip and lenient decode.
        Registry reg;
        Entry    a{ "Dress.esp|00000801", 56, "Red Dress", 0x800, { 0x801 } };
        Entry    b{ "Robe.esp|00000D62", 58, "Robe", 0x802, { 0x803, 0x804 } };
        reg.entries = { a, b };
        reg.nextId  = 0x806;
        const auto back = Decode(Encode(reg));
        CHECK(back.entries.size() == 2);
        CHECK(back.nextId == 0x806);
        if (back.entries.size() == 2) {
            CHECK(back.entries[1].source == "Robe.esp|00000D62" && back.entries[1].slot == 58);
            CHECK(back.entries[1].armas.size() == 2 && back.entries[1].armas[1] == 0x804);
        }
        CHECK(Find(back, "Robe.esp|00000D62", 58) != nullptr);
        CHECK(Find(back, "Robe.esp|00000D62", 56) == nullptr);

        Json::Value j = Encode(reg);
        j["nextId"] = 0x801;               // stale: must end above every kept ID
        Json::Value bad(Json::objectValue);  // reuses an ID
        bad["source"] = "X.esp|00000001";
        bad["slot"]   = 60;
        bad["armo"]   = 0x801;
        j["copies"].append(bad);
        Json::Value slot32(Json::objectValue);  // not a layer slot
        slot32["source"] = "Y.esp|00000001";
        slot32["slot"]   = 32;
        slot32["armo"]   = 0x900;
        j["copies"].append(slot32);
        Json::Value dup = j["copies"][0];       // same source + slot again
        dup["armo"]     = 0x950;
        dup["armas"]    = Json::Value(Json::arrayValue);
        j["copies"].append(dup);
        const auto lenient = Decode(j);
        CHECK(lenient.entries.size() == 2);
        CHECK(lenient.nextId == 0x805);
        CHECK(Decode(Json::Value(5)).entries.empty());
    }

    if (g_failures == 0) {
        std::printf("all LayerCopyModel tests passed\n");
        return 0;
    }
    std::printf("%d LayerCopyModel check(s) failed\n", g_failures);
    return 1;
}
