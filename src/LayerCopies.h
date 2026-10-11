#pragma once

// Layer copies (addon, experimental). See LayerCopyModel.h. Writes
// "FittingRoom Layers.esp": for each copy, the source piece's ARMO and every
// ARMA, unchanged except that they use one free slot, so the copy can be worn
// (styled) together with a piece that uses the source's own slot. Copies show
// up after the plugin is enabled and the game restarted; they are unlocked in
// the look collection whenever their source is.
//
// Add/Remove/List: the editor's present thread (they read static form data,
// as the rest of the editor does). Tick: main thread.

#include "Outfit.h"  // StyleRefKey

#include <cstdint>
#include <string>
#include <vector>

namespace OS::LayerCopies {

    struct Row {
        std::string   name;         // the copy's name, "Dress [slot 56]"
        std::uint32_t slot{ 0 };
        bool          loaded{ false };       // the copy exists in this session
        bool          sourceFound{ false };  // its source piece is in the load order
    };

    [[nodiscard]] std::vector<Row> List();

    // Makes the copy of a_source moved to a_slot (44..61), or finds the one
    // already made, and rewrites the plugin. a_message says what happened and
    // what to do next.
    bool Add(const StyleRefKey& a_source, std::uint32_t a_slot, std::string& a_message);

    // Removes the copy at a_index (List order) and rewrites the plugin.
    bool Remove(std::size_t a_index, std::string& a_message);

    // Main thread, every couple of seconds: copies whose source look you own
    // are added to the look collection.
    void Tick();

}  // namespace OS::LayerCopies
