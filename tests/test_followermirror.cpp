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

    if (g_failures == 0) {
        std::printf("all FollowerMirror tests passed\n");
        return 0;
    }
    std::printf("%d FollowerMirror check(s) failed\n", g_failures);
    return 1;
}
