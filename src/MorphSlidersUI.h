#pragma once

// One list of body-slider morph controls (addon), shared by "Make real armor"
// and "Shape pieces". FLICK present thread.

#include <map>
#include <string>
#include <vector>

namespace OS::MorphSlidersUI {

    // a_names: the morphs the piece's tri carries. a_values: name -> value
    // (1 = 100%; a value dragged to zero is removed). a_filter: the find box's
    // text, kept by the caller. True when a value changed this frame.
    bool Draw(const std::vector<std::string>& a_names, std::map<std::string, float>& a_values,
              std::string& a_filter, float a_em);

    // How many values are non-zero.
    [[nodiscard]] std::size_t Changed(const std::map<std::string, float>& a_values);

}  // namespace OS::MorphSlidersUI
