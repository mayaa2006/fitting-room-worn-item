#pragma once

// "Make real armor" export (addon). Turns the outfit shown on the player into
// up to four real, equippable items - Torso, Helmet, Gloves, Boots - each a
// merged NIF with the outfit's dyes baked into its textures, plus a small
// ESL-flagged plugin holding the ARMO/ARMA records and a free forge recipe.
//
// Threads, in order:
//   Start            any thread (the editor calls it from FLICK's present thread)
//   Gather           main thread (SKSE task): reads the biped, forms, live 3D
//   PumpRenderThread render thread: copies the dyed GPU textures to the CPU
//   Worker           its own thread: BC7 compression, NIF rewrite/merge,
//                    the plugin, all file writes
// Status is readable from any thread at any time.

#include "ArmorExportModel.h"
#include "Outfit.h"  // Outfit, StyleRefKey

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace OS::ArmorExport {

    using ArmorExportModel::ArmorType;
    using ArmorExportModel::Bucket;

    // What the user sets per item in the menu.
    struct ItemSettings {
        bool        include{ true };
        std::string name;
        ArmorType   type{ ArmorType::kLight };
        float       rating{ 0.0f };
        float       weight{ 0.0f };
        int         value{ 0 };
    };

    // One item: a bucket and the styled pieces that go into it.
    struct ItemPlan {
        Bucket                   bucket{ Bucket::kTorso };
        std::vector<StyleRefKey> styles;
        std::vector<std::string> pieceNames;  // for the menu
        std::uint32_t            slots{ 0 };  // union of the pieces' slot masks
        ItemSettings             defaults;
    };

    struct Plan {
        std::vector<ItemPlan>    items;    // Torso, Helmet, Gloves, Boots order; empty ones left out
        std::vector<std::string> skipped;  // pieces left out, with why
    };

    // Groups an outfit's styled pieces into the four items and fills default
    // stats (summed from the pieces' own records). Reads static form data
    // only, so it is safe from the editor's draw thread.
    [[nodiscard]] Plan PlanFor(const Outfit& a_outfit, std::string_view a_outfitName);

    struct Request {
        std::string                                     pluginName;  // without ".esp"
        std::vector<std::pair<ItemPlan, ItemSettings>> items;
        bool                                            recipe{ true };
    };

    // False when an export is already running.
    bool Start(Request a_request);

    struct Status {
        bool        busy{ false };
        // Copying dyed textures off the GPU: that step needs the editor's
        // render hook, so the window must stay open until it is over.
        bool        copying{ false };
        bool        finished{ false };
        bool        ok{ false };
        std::string message;
    };
    [[nodiscard]] Status GetStatus();

    // Clears a finished status (the menu's "Done").
    void Acknowledge();

    // Render thread. Cheap no-op unless an export is waiting on a readback.
    void PumpRenderThread();

}  // namespace OS::ArmorExport
