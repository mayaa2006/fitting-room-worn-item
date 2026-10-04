#pragma once

// The "Make real armor" window (addon). Both calls on FLICK's present thread,
// from EditorUI. Draw must run at the editor window's own id scope (where the
// other modals are drawn), or the popup opened by Open never matches.

#include "Outfit.h"

namespace OS::ArmorExportUI {

    // Prepares the window for this outfit (what the player shows) and asks
    // for it to open on the next Draw.
    void Open(const Outfit& a_outfit);

    void Draw();

}  // namespace OS::ArmorExportUI
