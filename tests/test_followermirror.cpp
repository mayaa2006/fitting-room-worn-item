// Pure-logic tests for follower mirroring (addon). No engine, no RE:: types.
#include "FollowerMirror.h"

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
    using namespace OS::FollowerMirror;
    using Rules::Base;
    using Rules::BaseKind;
    using Rules::Rule;

    const Base town{ BaseKind::kOutfit, "Town" };
    const Base real{ BaseKind::kRealGear, "" };
    const Base keep{ BaseKind::kKeep, "" };

    {  // TargetFor: only a flagged rule with a base steers followers.
        Rule r;
        r.id = "r";
        CHECK(!TargetFor(nullptr, town));
        CHECK(!TargetFor(&r, town));  // flag off
        r.followers = true;
        const auto t = TargetFor(&r, town);
        CHECK(t.has_value());
        if (t) {
            CHECK(!t->realGear);
            CHECK(t->outfitName == "Town");
        }
        const auto g = TargetFor(&r, real);
        CHECK(g.has_value() && g->realGear && g->outfitName.empty());
        CHECK(!TargetFor(&r, keep));  // kKeep names no outfit
        CHECK(!TargetFor(&r, Base{ BaseKind::kOutfit, "" }));
    }

    {  // FindOutfit: exact beats case-insensitive; no match is -1.
        OutfitLibrary lib;
        lib.Create("town");     // 0
        lib.Create("Dungeon");  // 1
        lib.Create("Town");     // 2
        CHECK(FindOutfit(lib, "Town") == 2);     // exact, even though a loose match is earlier
        CHECK(FindOutfit(lib, "TOWN") == 0);     // loose: first case-insensitive hit
        CHECK(FindOutfit(lib, "dungeon") == 1);
        CHECK(FindOutfit(lib, "Tavern") == -1);
        CHECK(FindOutfit(lib, "") == -1);
        CHECK(FindOutfit(OutfitLibrary{}, "Town") == -1);
        CHECK(FindOutfit(lib, "Town ") == -1);   // no trimming: names are compared as typed
    }

    {  // PlanFor + Apply, outfit target.
        OutfitLibrary lib;
        lib.Create("Dungeon");  // 0
        lib.Create("Town");     // 1
        const Target t{ false, "Town" };

        auto p = PlanFor(lib, t);  // nothing active yet
        CHECK(p.action == Action::kActivate && p.index == 1);
        CHECK(Apply(lib, p));
        CHECK(lib.ActiveIndex() == 1);

        p = PlanFor(lib, t);  // now already there
        CHECK(p.action == Action::kAlready);
        CHECK(!Apply(lib, p));
        CHECK(lib.ActiveIndex() == 1);

        lib.Activate(0);  // she was switched by hand
        p = PlanFor(lib, t);
        CHECK(p.action == Action::kActivate && p.index == 1);

        // A follower without the outfit is left exactly as she is.
        const Target missing{ false, "Tavern" };
        p = PlanFor(lib, missing);
        CHECK(p.action == Action::kMissing);
        CHECK(!Apply(lib, p));
        CHECK(lib.ActiveIndex() == 0);
    }

    {  // PlanFor + Apply, real-gear target.
        OutfitLibrary lib;
        lib.Create("Town");
        const Target g{ true, {} };
        CHECK(PlanFor(lib, g).action == Action::kAlready);  // nothing active = real gear
        lib.Activate(0);
        const auto p = PlanFor(lib, g);
        CHECK(p.action == Action::kDeactivate);
        CHECK(Apply(lib, p));
        CHECK(lib.ActiveIndex() == -1);
    }

    {  // Target equality drives the "did the target move" serial bump.
        CHECK((Target{ false, "Town" } == Target{ false, "Town" }));
        CHECK(!(Target{ false, "Town" } == Target{ false, "Dungeon" }));
        CHECK(!(Target{ true, {} } == Target{ false, {} }));
    }

    {  // SyncStep: the scenarios from the 2026-10-04 bug hunt.
        auto lib = [] {
            OutfitLibrary l;
            l.Create("Housecarl");  // 0 - her own
            l.Create("Town");       // 1
            l.Activate(0);
            return l;
        };
        const Target town{ false, "Town" };
        const Target dungeon{ false, "Dungeon" };  // she has none
        const Target real{ true, {} };

        // Real gear, then a rule she has no outfit for: back to Housecarl,
        // not stuck in real gear.
        {
            auto           l = lib();
            FollowerRecord r;
            CHECK(SyncStep(l, r, real) == SyncResult::kChanged);
            CHECK(l.ActiveIndex() == -1);
            CHECK(r.mirrored && r.own == (Target{ false, "Housecarl" }));
            CHECK(SyncStep(l, r, dungeon) == SyncResult::kRestored);
            CHECK(l.ActiveIndex() == 0);
            CHECK(!r.mirrored);
            CHECK(r.synced == dungeon);
        }
        // Town -> Dungeon -> Town: she follows, goes home in between, follows again.
        {
            auto           l = lib();
            FollowerRecord r;
            CHECK(SyncStep(l, r, town) == SyncResult::kChanged && l.ActiveIndex() == 1);
            CHECK(SyncStep(l, r, dungeon) == SyncResult::kRestored && l.ActiveIndex() == 0);
            CHECK(SyncStep(l, r, town) == SyncResult::kChanged && l.ActiveIndex() == 1);
            CHECK(r.own == (Target{ false, "Housecarl" }));  // remembered across the round trip
        }
        // A hand pick under Town survives a target she lacks, but the NEXT
        // change to a target she has re-dresses her.
        {
            auto           l = lib();
            FollowerRecord r;
            SyncStep(l, r, town);
            l.Activate(0);  // user dresses her in Housecarl by hand...
            r.mirrored = false;
            r.synced   = town;  // ...which NotifyFollowerDressed records
            CHECK(SyncStep(l, r, dungeon) == SyncResult::kNoChange);
            CHECK(l.ActiveIndex() == 0);           // hand pick kept
            CHECK(r.synced == dungeon);            // but the target has moved on
            CHECK(SyncStep(l, r, town) == SyncResult::kChanged && l.ActiveIndex() == 1);
        }
        // Already wearing the target: nothing to do, and she is not marked as
        // mirror-dressed (her look is still her own).
        {
            auto l = lib();
            l.Activate(1);
            FollowerRecord r;
            CHECK(SyncStep(l, r, town) == SyncResult::kNoChange);
            CHECK(!r.mirrored);
        }
        // Her own outfit was deleted meanwhile: restoring finds nothing and
        // leaves her as she is rather than stripping her.
        {
            auto           l = lib();
            FollowerRecord r;
            SyncStep(l, r, town);
            l.Remove(0);  // Housecarl gone; Town is now index 0 and active
            CHECK(l.ActiveIndex() == 0);
            CHECK(SyncStep(l, r, dungeon) == SyncResult::kNoChange);
            CHECK(l.ActiveIndex() == 0);
        }
    }

    if (g_failures == 0) {
        std::printf("all FollowerMirror tests passed\n");
        return 0;
    }
    std::printf("%d FollowerMirror check(s) failed\n", g_failures);
    return 1;
}
