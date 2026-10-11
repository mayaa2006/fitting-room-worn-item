#pragma once

// The "Layer pieces" window (addon, experimental): make copies of pieces that
// use a free slot, so two pieces of one slot can be worn together. FLICK's
// present thread, from EditorUI; Draw at the editor window's id scope.

#include "Outfit.h"

namespace OS::LayerCopyUI {

    void Open(const Outfit& a_outfit);
    void Draw();

}  // namespace OS::LayerCopyUI
