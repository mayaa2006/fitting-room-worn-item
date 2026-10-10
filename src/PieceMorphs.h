#pragma once

// Live per-piece body morphs (addon, experimental). See PieceMorphModel.h for
// the trick; this is the engine half:
//
//   * the settings, kept in Data/SKSE/Plugins/FittingRoom/PieceMorphs.json
//     (per piece of armor, for every character),
//   * on the world heartbeat: for each piece on the player that has settings,
//     write its tri copy (once, on a worker thread), point the piece's live
//     BODYTRI link at it, and keep the prefixed values under Fitting Room's
//     RaceMenu key, re-applying after anything (OBody) clears them.
//
// Get/Set/StateOf are for the menu and safe from any thread. Tick is main
// thread only.

#include "Outfit.h"  // StyleRefKey

#include <map>
#include <string>

namespace RE {
    class Actor;
}

namespace OS::PieceMorphs {

    // What the menu can say about a piece that has settings.
    enum class State {
        kNotWorn,       // not showing on your character right now
        kPreparing,     // writing its tri copy
        kLive,          // linked, values applied
        kNoSliders,     // the worn mesh has no body-slider (BODYTRI) link
        kFailed,        // its tri could not be read or the copy not written
        kPathTooShort,  // the live link's text is too short to rewrite in place
    };

    [[nodiscard]] std::map<std::string, float> Get(const StyleRefKey& a_piece);
    // Empty a_values removes the piece's settings. Takes effect on the next
    // heartbeat; saved to the file a second after the last change.
    void Set(const StyleRefKey& a_piece, const std::string& a_label, std::map<std::string, float> a_values);
    [[nodiscard]] State StateOf(const StyleRefKey& a_piece);

    // Main thread, from the world heartbeat. Cheap when nothing has settings.
    void Tick(RE::Actor* a_player);

}  // namespace OS::PieceMorphs
