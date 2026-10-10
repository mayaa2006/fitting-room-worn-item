#include "PieceMorphUI.h"

#include "ArmorExport.h"
#include "ChamferPanel.h"
#include "FuckCompat.h"
#include "MorphSlidersUI.h"
#include "PieceMorphs.h"
#include "RaceMenuMorphApi.h"

#include <chrono>
#include <future>
#include <map>
#include <string>
#include <vector>

namespace OS::PieceMorphUI {

    namespace {
        constexpr const char* kModalId = "shape_pieces";

        struct Piece {
            StyleRefKey                  key;
            std::string                  name;
            std::vector<std::string>     triPaths;
            std::map<std::string, float> values;
            std::string                  filter;
        };

        bool                                         g_requestOpen = false;
        std::vector<Piece>                           g_pieces;
        std::future<std::vector<std::vector<std::string>>> g_namesJob;
        std::vector<std::vector<std::string>>        g_names;
        bool                                         g_namesReady = false;

        const char* StateText(PieceMorphs::State a_s) {
            switch (a_s) {
            case PieceMorphs::State::kNotWorn:      return "not showing on your character right now";
            case PieceMorphs::State::kPreparing:    return "preparing...";
            case PieceMorphs::State::kLive:         return "live";
            case PieceMorphs::State::kNoSliders:    return "the worn mesh has no body sliders";
            case PieceMorphs::State::kFailed:       return "could not prepare it (see FittingRoom.log)";
            case PieceMorphs::State::kPathTooShort: return "cannot be relinked (see FittingRoom.log)";
            }
            return "";
        }
    }

    void Open(const Outfit& a_outfit) {
        const auto plan = ArmorExport::PlanFor(a_outfit, a_outfit.name);
        g_pieces.clear();
        std::vector<std::vector<std::string>> paths;
        for (const auto& item : plan.items) {
            for (std::size_t i = 0; i < item.styles.size(); ++i) {
                Piece p;
                p.key      = item.styles[i];
                p.name     = i < item.pieceNames.size() ? item.pieceNames[i] : "Piece";
                p.triPaths = i < item.triPaths.size() ? item.triPaths[i] : std::vector<std::string>{};
                p.values   = PieceMorphs::Get(p.key);
                paths.push_back(p.triPaths);
                g_pieces.push_back(std::move(p));
            }
        }
        g_names.clear();
        g_namesReady = false;
        g_namesJob   = std::async(std::launch::async, [paths = std::move(paths)] {
            std::vector<std::vector<std::string>> out;
            for (const auto& p : paths) {
                out.push_back(ArmorExport::MorphNames(p));
            }
            return out;
        });
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
        if (!g_namesReady && g_namesJob.valid() &&
            g_namesJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            g_names      = g_namesJob.get();
            g_namesReady = true;
        }
        const float em = OS::ui::FontSize();

        FUCK::TextUnformatted("Shape pieces (experimental)");
        FUCK::PushTextWrapPos(em * 34.0f);
        FUCK::TextDisabled("%s",
                           "Changes one piece's shape while you wear it, on top of your body sliders. "
                           "It sticks to the piece of armor, in every outfit and on every character.");
        FUCK::PopTextWrapPos();
        FUCK::Separator();

        if (!RaceMenuMorphApi::Available()) {
            FUCK::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s",
                              "RaceMenu's body morphs are not available, so nothing here can move.");
        }
        if (g_pieces.empty()) {
            FUCK::TextWrapped("%s", "This outfit has no styled pieces.");
        }

        for (std::size_t i = 0; i < g_pieces.size(); ++i) {
            auto& p = g_pieces[i];
            FUCK::PushID(static_cast<int>(i));
            const auto  changed = MorphSlidersUI::Changed(p.values);
            std::string label   = p.name;
            if (changed > 0) {
                label += " (" + std::to_string(changed) + " changed, " +
                         StateText(PieceMorphs::StateOf(p.key)) + ")";
            }
            label += "###piece";
            if (FUCK::TreeNode(label.c_str())) {
                const std::vector<std::string>* names =
                    g_namesReady && i < g_names.size() ? &g_names[i] : nullptr;
                if (!g_namesReady) {
                    FUCK::TextDisabled("%s", "Reading the piece's body-slider data...");
                } else if (!names || names->empty()) {
                    FUCK::PushTextWrapPos(em * 34.0f);
                    FUCK::TextWrapped("%s",
                                      "This piece has no body-slider data (it was not built in BodySlide "
                                      "with morphs), so its shape cannot be changed.");
                    FUCK::PopTextWrapPos();
                } else if (MorphSlidersUI::Draw(*names, p.values, p.filter, em)) {
                    PieceMorphs::Set(p.key, p.name, p.values);
                }
                FUCK::TreePop();
            }
            FUCK::PopID();
        }

        FUCK::Spacing();
        if (ChamferPanel::Button("Close").clicked) {
            FUCK::CloseCurrentPopup();
        }
        FUCK::EndPopup();
    }

}  // namespace OS::PieceMorphUI
