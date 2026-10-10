#pragma once

// The "Shape pieces" window (addon, experimental): live per-piece body morphs
// on the outfit your character is wearing. Both calls on FLICK's present
// thread, from EditorUI; Draw at the editor window's own id scope, like
// ArmorExportUI.

#include "Outfit.h"

namespace OS::PieceMorphUI {

    void Open(const Outfit& a_outfit);
    void Draw();

}  // namespace OS::PieceMorphUI
