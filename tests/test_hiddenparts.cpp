// Tests for hidden parts (addon): the flag on the outfit, the outfit JSON,
// and the co-save side record that keeps 'LIBR' at the upstream format.
#include "HiddenPartsCodec.h"
#include "JsonCodec.h"
#include "PersistenceCodec.h"
#include "SlotMask.h"

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
    const StyleRefKey dress{ "Dress.esp", 0x801 };
    const StyleRefKey robe{ "Robe.esp", 0xD62 };

    {  // The flag on the outfit: independent of colour, kept by colour edits.
        Outfit o;
        o.name = "Test";
        o.SetStyle(kBitBody, dress);
        CHECK(!o.DyeFor(kBitBody).Any());
        o.SetDyeHidden(kBitBody, static_cast<DyeChannelId>(1), true);
        CHECK(o.DyeFor(kBitBody).Any());  // hidden alone is something to keep
        CHECK(o.DyeFor(kBitBody).HiddenMask() == 0x2);
        CHECK(!o.DyeFor(kBitBody).channels[1].set);
        // Dye that part and erase its colour again: still hidden.
        o.SetDye(kBitBody, static_cast<DyeChannelId>(1), DyeChannel{ true, 200, 10, 10 });
        CHECK(o.DyeFor(kBitBody).channels[1].hidden && o.DyeFor(kBitBody).channels[1].set);
        o.SetDye(kBitBody, static_cast<DyeChannelId>(1), DyeChannel{});
        CHECK(o.DyeFor(kBitBody).channels[1].hidden && !o.DyeFor(kBitBody).channels[1].set);
        // Showing it again with no colour left drops the stored entry.
        o.SetDyeHidden(kBitBody, static_cast<DyeChannelId>(1), false);
        CHECK(!o.DyeFor(kBitBody).Any());
        CHECK(o.ArmourDyeCount() == 0);
        // A hide is an edit the editor has to be able to commit.
        Outfit base = o;
        o.SetDyeHidden(kBitBody, static_cast<DyeChannelId>(0), true);
        CHECK(DyeDiffers(base, o));
        // The garment is in the key, like a colour: restyle, and it is the
        // other garment's (unhidden) parts that show.
        o.SetStyle(kBitBody, robe);
        CHECK(o.DyeFor(kBitBody).HiddenMask() == 0);
        o.SetStyle(kBitBody, dress);
        CHECK(o.DyeFor(kBitBody).HiddenMask() == 0x1);
        // Set for a garment not in the slot.
        o.SetArmourHiddenMask(kBitBody, robe, 0x5);
        o.SetStyle(kBitBody, robe);
        CHECK(o.DyeFor(kBitBody).HiddenMask() == 0x5);
        o.SetArmourHiddenMask(kBitBody, robe, 0);
        CHECK(o.DyeFor(kBitBody).HiddenMask() == 0);
    }

    {  // Outfit JSON carries it; a colourless hidden part survives too.
        Outfit o;
        o.name = "Json";
        o.SetStyle(kBitBody, dress);
        o.SetDye(kBitBody, static_cast<DyeChannelId>(0), DyeChannel{ true, 1, 2, 3 });
        o.SetDyeHidden(kBitBody, static_cast<DyeChannelId>(0), true);
        o.SetDyeHidden(kBitBody, static_cast<DyeChannelId>(4), true);
        const auto j = JsonCodec::OutfitToJson(o);
        Outfit back;
        CHECK(JsonCodec::JsonToOutfit(j, back));
        CHECK(back.DyeFor(kBitBody).HiddenMask() == 0x11);
        CHECK(back.DyeFor(kBitBody).channels[0].set && back.DyeFor(kBitBody).channels[0].r == 1);
        // An outfit with nothing hidden writes no "hidden" key at all.
        Outfit plain;
        plain.name = "Plain";
        plain.SetStyle(kBitBody, dress);
        plain.SetDye(kBitBody, static_cast<DyeChannelId>(0), DyeChannel{ true, 1, 2, 3 });
        const auto pj = JsonCodec::OutfitToJson(plain);
        CHECK(pj.isMember("dyes") && !pj["dyes"][0].isMember("hidden"));
    }

    {  // LIBR stays the upstream format; the side record puts the flags back.
        OutfitLibrary lib;
        lib.Create("A");
        lib.Create("B");
        auto* b = lib.At(1);
        b->SetStyle(kBitBody, dress);
        b->SetDye(kBitBody, static_cast<DyeChannelId>(2), DyeChannel{ true, 9, 9, 9 });
        b->SetDyeHidden(kBitBody, static_cast<DyeChannelId>(2), true);
        b->SetDyeHidden(kBitBody, static_cast<DyeChannelId>(3), true);  // colourless

        const auto bytes = Encode(lib);
        OutfitLibrary loaded;
        CHECK(Decode(bytes, kCodecVersion, loaded));
        CHECK(loaded.Count() == 2);
        CHECK(loaded.At(1)->DyeFor(kBitBody).HiddenMask() == 0);  // LIBR knows nothing of it
        CHECK(loaded.At(1)->DyeFor(kBitBody).channels[2].set);     // the colour came back

        const auto side = HiddenPartsCodec::EncodeLibrary(lib);
        CHECK(side.size() == 1);
        CHECK(HiddenPartsCodec::ApplyLibrary(side, loaded) == 1);
        CHECK(loaded.At(1)->DyeFor(kBitBody).HiddenMask() == 0xC);
        CHECK(loaded.At(1)->DyeFor(kBitBody).channels[2].set);

        // Found by name when the library was reordered.
        OutfitLibrary moved;
        moved.Create("B");
        moved.Create("A");
        moved.At(0)->SetStyle(kBitBody, dress);
        CHECK(HiddenPartsCodec::ApplyLibrary(side, moved) == 1);
        CHECK(moved.At(0)->DyeFor(kBitBody).HiddenMask() == 0xC);
        // And not applied anywhere when the name is gone or ambiguous.
        OutfitLibrary gone;
        gone.Create("C");
        CHECK(HiddenPartsCodec::ApplyLibrary(side, gone) == 0);
        CHECK(HiddenPartsCodec::ApplyLibrary(Json::Value(3), gone) == 0);
        // Nothing hidden, nothing written.
        CHECK(HiddenPartsCodec::EncodeLibrary(loaded).size() == 1);
        OutfitLibrary none;
        none.Create("Z");
        CHECK(HiddenPartsCodec::EncodeLibrary(none).empty());
    }

    if (g_failures == 0) {
        std::printf("all HiddenParts tests passed\n");
        return 0;
    }
    std::printf("%d HiddenParts check(s) failed\n", g_failures);
    return 1;
}
