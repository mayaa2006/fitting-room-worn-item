#pragma once

// Follower mirroring (addon): pure logic, no engine types, so it compiles into
// the FollowerMirrorTests executable exactly like RuleModel.h does.
//
// The feature in one sentence: when a rule marked "Also dress followers" sets
// the player's outfit, every current follower who owns an outfit of the SAME
// NAME in her own Fitting Room library switches to it too. A follower without
// one is left alone. The engine-facing half lives in WorldWatch.cpp
// (SyncFollowers) and OutfitSession::SyncNpc.
//
// ⚠ IT MIRRORS THE BASE AND NOTHING ELSE. Overlays (hide headgear, restyle a
// slot) are a player-only concept in OutfitSession - there is no per-NPC
// overlay to write them into - so a follower gets the rule's OUTFIT, not its
// slot tweaks. And a kKeep base mirrors nothing, which is also what kKeep
// means for the player: "this rule expresses no opinion about the outfit".

#include "Outfit.h"     // OutfitLibrary
#include "RuleModel.h"  // Rules::Rule, Rules::Base

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace OS::FollowerMirror {

    // What followers should be wearing right now, derived from the rule that
    // currently owns the player's base.
    struct Target {
        bool        realGear{ false };
        std::string outfitName;  // empty when realGear

        friend bool operator==(const Target&, const Target&) = default;
    };

    // The mirror target for a base that a rule just applied, or nullopt when
    // followers should be left alone: no rule, a rule without the follower
    // flag, or a kKeep base (which names no outfit at all).
    [[nodiscard]] inline std::optional<Target> TargetFor(const Rules::Rule* a_rule,
                                                         const Rules::Base& a_base) {
        if (!a_rule || !a_rule->followers) {
            return std::nullopt;
        }
        switch (a_base.kind) {
        case Rules::BaseKind::kRealGear:
            return Target{ true, {} };
        case Rules::BaseKind::kOutfit:
            if (a_base.outfitName.empty()) {
                return std::nullopt;
            }
            return Target{ false, a_base.outfitName };
        case Rules::BaseKind::kKeep:
            return std::nullopt;
        }
        return std::nullopt;
    }

    namespace detail {
        [[nodiscard]] inline bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
            if (a.size() != b.size()) {
                return false;
            }
            for (std::size_t i = 0; i < a.size(); ++i) {
                auto lower = [](char c) {
                    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
                };
                if (lower(a[i]) != lower(b[i])) {
                    return false;
                }
            }
            return true;
        }
    }

    // Index of the follower's outfit that answers to a_name, or -1. An EXACT
    // match wins; failing that, the first case-insensitive one, because the
    // two libraries are typed by hand at different times and "town" on the
    // follower meaning "Town" on the player is the obvious intent.
    [[nodiscard]] inline int FindOutfit(const OutfitLibrary& a_lib, std::string_view a_name) {
        if (a_name.empty()) {
            return -1;
        }
        int loose = -1;
        for (std::size_t i = 0; i < a_lib.Count(); ++i) {
            const auto* o = a_lib.At(i);
            if (!o) {
                continue;
            }
            if (o->name == a_name) {
                return static_cast<int>(i);
            }
            if (loose < 0 && detail::EqualsIgnoreCase(o->name, a_name)) {
                loose = static_cast<int>(i);
            }
        }
        return loose;
    }

    enum class Action : std::uint8_t {
        kMissing,     // she has no outfit by that name: leave her alone
        kAlready,     // already wearing exactly what the target asks for
        kActivate,    // switch her to outfit `index`
        kDeactivate,  // real gear: clear her active outfit
    };

    struct Plan {
        Action action{ Action::kMissing };
        int    index{ -1 };  // kActivate only

        friend bool operator==(const Plan&, const Plan&) = default;
    };

    // What to do to one follower's library to make it match a_target.
    [[nodiscard]] inline Plan PlanFor(const OutfitLibrary& a_lib, const Target& a_target) {
        if (a_target.realGear) {
            return a_lib.ActiveIndex() < 0 ? Plan{ Action::kAlready, -1 }
                                           : Plan{ Action::kDeactivate, -1 };
        }
        const int idx = FindOutfit(a_lib, a_target.outfitName);
        if (idx < 0) {
            return Plan{ Action::kMissing, -1 };
        }
        if (a_lib.ActiveIndex() == idx) {
            return Plan{ Action::kAlready, idx };
        }
        return Plan{ Action::kActivate, idx };
    }

    // Applies a plan to a library. Returns true when it changed the selection.
    inline bool Apply(OutfitLibrary& a_lib, const Plan& a_plan) {
        switch (a_plan.action) {
        case Action::kActivate:
            if (a_plan.index < 0) {
                return false;
            }
            a_lib.Activate(static_cast<std::size_t>(a_plan.index));
            return true;
        case Action::kDeactivate:
            a_lib.Deactivate();
            return true;
        case Action::kMissing:
        case Action::kAlready:
            return false;
        }
        return false;
    }

    // What she is wearing now, as a target: her active outfit, or real gear.
    [[nodiscard]] inline Target CurrentSelection(const OutfitLibrary& a_lib) {
        const auto* o = a_lib.Active();
        return o ? Target{ false, o->name } : Target{ true, {} };
    }

    // Per follower, what the mirror knows about her. Saved in the co-save.
    struct FollowerRecord {
        std::optional<Target> synced;    // the target she was last brought in line with
        bool                  mirrored{ false };  // her current outfit was put on by the mirror
        Target                own{ true, {} };    // what she wore before the mirror first changed her

        friend bool operator==(const FollowerRecord&, const FollowerRecord&) = default;
    };

    enum class SyncResult : std::uint8_t {
        kNoChange,  // nothing about her look changed
        kChanged,   // switched to the target
        kRestored,  // the target has nothing for her, so she went back to her own look
    };

    // One follower, one target: the whole per-follower decision, pure.
    //  - She has the target outfit: switch, remembering her own look the first
    //    time the mirror changes her.
    //  - She does NOT have it (2026-10-04 review: a "real gear" rule followed
    //    by a rule she has no outfit for left her stuck in real gear): if the
    //    mirror dressed her, she goes back to her own look; if she dressed
    //    herself (by hand), she is left alone.
    // Either way she is marked as synced to this target, so a hand pick stays
    // only until the target moves on.
    inline SyncResult SyncStep(OutfitLibrary& a_lib, FollowerRecord& a_rec, const Target& a_target) {
        const auto plan = PlanFor(a_lib, a_target);
        a_rec.synced    = a_target;
        switch (plan.action) {
        case Action::kAlready:
            return SyncResult::kNoChange;
        case Action::kMissing: {
            if (!a_rec.mirrored) {
                return SyncResult::kNoChange;
            }
            a_rec.mirrored = false;
            const auto back = PlanFor(a_lib, a_rec.own);
            return Apply(a_lib, back) ? SyncResult::kRestored : SyncResult::kNoChange;
        }
        case Action::kActivate:
        case Action::kDeactivate:
            if (!a_rec.mirrored) {
                a_rec.own      = CurrentSelection(a_lib);
                a_rec.mirrored = true;
            }
            return Apply(a_lib, plan) ? SyncResult::kChanged : SyncResult::kNoChange;
        }
        return SyncResult::kNoChange;
    }

}  // namespace OS::FollowerMirror
