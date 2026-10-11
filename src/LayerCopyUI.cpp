#include "LayerCopyUI.h"

#include "ArmorExport.h"
#include "ChamferPanel.h"
#include "FuckCompat.h"
#include "LayerCopies.h"
#include "LayerCopyModel.h"
#include "StyleRef.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

namespace OS::LayerCopyUI {

    namespace {
        namespace L = LayerCopyModel;

        constexpr const char* kModalId = "layer_pieces";

        bool                     g_requestOpen = false;
        std::uint32_t            g_usedMask    = 0;
        // Pickable pieces: this outfit's first, then every named armor.
        std::vector<std::string> g_labels;
        std::vector<StyleRefKey> g_keys;
        int                      g_piece = -1;
        int                      g_slotIndex = 0;
        std::string              g_message;
        bool                     g_messageOk = true;
        std::vector<LayerCopies::Row> g_rows;

        void BuildChoices(const Outfit& a_outfit) {
            g_labels.clear();
            g_keys.clear();
            const auto plan = ArmorExport::PlanFor(a_outfit, a_outfit.name);
            g_usedMask      = 0;
            for (const auto& item : plan.items) {
                g_usedMask |= item.slots;
                for (std::size_t i = 0; i < item.styles.size(); ++i) {
                    g_keys.push_back(item.styles[i]);
                    g_labels.push_back("(this outfit) " +
                                       (i < item.pieceNames.size() ? item.pieceNames[i] : std::string("Piece")));
                }
            }
            const auto outfitCount = g_keys.size();
            std::vector<std::pair<std::string, StyleRefKey>> rows;
            if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                for (auto* armo : dh->GetFormArray<RE::TESObjectARMO>()) {
                    const char* full = armo ? armo->GetFullName() : nullptr;
                    if (!full || !*full) {
                        continue;
                    }
                    StyleRefKey key;
                    if (!StyleRef::Make(armo, key)) {
                        continue;
                    }
                    rows.emplace_back(std::string(full) + "  (" + key.modName + ")", std::move(key));
                }
            }
            std::ranges::sort(rows, [](const auto& a, const auto& b) { return a.first < b.first; });
            for (auto& [label, key] : rows) {
                g_labels.push_back(std::move(label));
                g_keys.push_back(std::move(key));
            }
            g_piece = outfitCount > 0 ? 0 : -1;
        }

        std::string SlotLabel(const L::SlotInfo& a_s) {
            std::string out = std::to_string(a_s.slot) + " - " + a_s.name;
            if (g_usedMask & L::SlotBit(a_s.slot)) {
                out += " (used by this outfit)";
            }
            return out;
        }
    }

    void Open(const Outfit& a_outfit) {
        BuildChoices(a_outfit);
        const auto suggested = L::SuggestSlot(g_usedMask);
        g_slotIndex          = 0;
        for (std::size_t i = 0; i < std::size(L::kLayerSlots); ++i) {
            if (L::kLayerSlots[i].slot == suggested) {
                g_slotIndex = static_cast<int>(i);
            }
        }
        g_message.clear();
        g_rows        = LayerCopies::List();
        g_requestOpen = true;
    }

    void Draw() {
        if (g_requestOpen) {
            g_requestOpen = false;
            OS::ui::OpenModal(kModalId);
        }
        if (!OS::ui::BeginModal(kModalId)) {
            return;
        }
        const float em = OS::ui::FontSize();

        FUCK::TextUnformatted("Layer pieces (experimental)");
        FUCK::PushTextWrapPos(em * 34.0f);
        FUCK::TextDisabled("%s",
                           "Skyrim shows one piece per slot, so two pieces that use the same slot replace each "
                           "other. A layer copy is the same piece moved to a free slot, so it can be worn on top. "
                           "Copies go into FittingRoom Layers.esp and appear after a restart.");
        FUCK::PopTextWrapPos();
        FUCK::Separator();

        FUCK::TextUnformatted("Piece to copy");
        FUCK::SetNextItemWidth(em * 24.0f);
        if (g_labels.empty()) {
            FUCK::TextDisabled("%s", "No named armor found.");
        } else {
            int sel = g_piece;
            if (FUCK::ComboWithFilter("##layerpiece", &sel, g_labels) && sel >= 0 &&
                sel < static_cast<int>(g_keys.size())) {
                g_piece = sel;
            }
        }

        FUCK::TextUnformatted("Move it to slot");
        std::vector<std::string> slotLabels;
        for (const auto& s : L::kLayerSlots) {
            slotLabels.push_back(SlotLabel(s));
        }
        FUCK::SetNextItemWidth(em * 24.0f);
        int slotSel = g_slotIndex;
        if (FUCK::Combo("##layerslot", &slotSel, slotLabels) && slotSel >= 0 &&
            slotSel < static_cast<int>(slotLabels.size())) {
            g_slotIndex = slotSel;
        }

        FUCK::Spacing();
        FUCK::BeginDisabled(g_piece < 0 || g_piece >= static_cast<int>(g_keys.size()));
        if (ChamferPanel::Button("Make layer copy").clicked) {
            g_messageOk = LayerCopies::Add(g_keys[static_cast<std::size_t>(g_piece)],
                                           L::kLayerSlots[static_cast<std::size_t>(g_slotIndex)].slot, g_message);
            g_rows = LayerCopies::List();
        }
        FUCK::EndDisabled();
        if (!g_message.empty()) {
            FUCK::PushTextWrapPos(em * 34.0f);
            if (!g_messageOk) {
                FUCK::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s", g_message.c_str());
            } else {
                FUCK::TextWrapped("%s", g_message.c_str());
            }
            FUCK::PopTextWrapPos();
        }

        if (!g_rows.empty()) {
            FUCK::Separator();
            FUCK::TextUnformatted("Your layer copies");
            for (std::size_t i = 0; i < g_rows.size(); ++i) {
                const auto& r = g_rows[i];
                FUCK::PushID(static_cast<int>(i));
                const char* state = !r.sourceFound ? "its mod is not loaded"
                                    : r.loaded     ? "ready"
                                                   : "restart the game to use it";
                FUCK::TextUnformatted((r.name + "  -  " + state).c_str());
                FUCK::SameLine();
                if (FUCK::Button("Remove")) {
                    g_messageOk = LayerCopies::Remove(i, g_message);
                    g_rows      = LayerCopies::List();
                    FUCK::PopID();
                    break;
                }
                if (FUCK::IsItemHovered()) {
                    FUCK::SetTooltip("Takes the copy out of the plugin. Outfits using it lose that piece "
                                     "after the next restart.");
                }
                FUCK::PopID();
            }
        }

        FUCK::Spacing();
        if (ChamferPanel::Button("Close").clicked) {
            FUCK::CloseCurrentPopup();
        }
        FUCK::EndPopup();
    }

}  // namespace OS::LayerCopyUI
