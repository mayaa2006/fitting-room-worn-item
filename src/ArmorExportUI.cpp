#include "ArmorExportUI.h"

#include "ArmorExport.h"
#include "ChamferPanel.h"
#include "FuckCompat.h"
#include "MorphSlidersUI.h"
#include "PieceMorphs.h"

#include <chrono>
#include <future>
#include <string>
#include <vector>

namespace OS::ArmorExportUI {

    namespace {
        namespace M = ArmorExportModel;

        constexpr const char* kModalId = "make_real_armor";

        bool                                g_requestOpen = false;
        ArmorExport::Plan                   g_plan;
        std::vector<ArmorExport::ItemSettings> g_settings;
        std::string                         g_pluginName;
        bool                                g_recipe = true;

        // Per item, per piece: the morph names its tri files carry. Read off
        // the draw thread (a body-slider tri is megabytes), once per opening.
        using MorphNameTable = std::vector<std::vector<std::vector<std::string>>>;
        std::future<MorphNameTable> g_namesJob;
        MorphNameTable              g_names;
        bool                        g_namesReady = false;
        // Per item, per piece: the morph list's filter text.
        std::vector<std::vector<std::string>> g_filters;

        void PollNames() {
            if (g_namesReady || !g_namesJob.valid()) {
                return;
            }
            if (g_namesJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                g_names      = g_namesJob.get();
                g_namesReady = true;
            }
        }

        // One piece's morph list. a_values is that piece's name -> value map.
        void DrawPieceMorphs(std::size_t a_item, std::size_t a_piece, const std::string& a_pieceName,
                             std::map<std::string, float>& a_values, float a_em) {
            const std::size_t changed = MorphSlidersUI::Changed(a_values);
            std::string label = "Body morphs: " + a_pieceName;
            if (changed > 0) {
                label += " (" + std::to_string(changed) + " changed)";
            }
            label += "###morphs";
            if (!FUCK::TreeNode(label.c_str())) {
                return;
            }
            const std::vector<std::string>* names = nullptr;
            if (g_namesReady && a_item < g_names.size() && a_piece < g_names[a_item].size()) {
                names = &g_names[a_item][a_piece];
            }
            if (!g_namesReady) {
                FUCK::TextDisabled("%s", "Reading the piece's body-slider data...");
            } else if (!names || names->empty()) {
                FUCK::PushTextWrapPos(a_em * 34.0f);
                FUCK::TextWrapped("%s",
                                  "This piece has no body-slider data (it was not built in BodySlide with "
                                  "morphs), so its shape cannot be changed here.");
                FUCK::PopTextWrapPos();
            } else {
                FUCK::PushTextWrapPos(a_em * 34.0f);
                FUCK::TextDisabled("%s",
                                   "Changes this piece only, on top of your body. The armor still follows "
                                   "your body sliders after this.");
                FUCK::PopTextWrapPos();
                (void)MorphSlidersUI::Draw(*names, a_values, g_filters[a_item][a_piece], a_em);
            }
            FUCK::TreePop();
        }

        int TypeIndex(M::ArmorType a_t) { return static_cast<int>(a_t); }
    }

    void Open(const Outfit& a_outfit) {
        g_plan = ArmorExport::PlanFor(a_outfit, a_outfit.name);
        g_settings.clear();
        g_filters.clear();
        std::vector<std::vector<std::vector<std::string>>> triPaths;
        for (const auto& it : g_plan.items) {
            g_settings.push_back(it.defaults);
            // Starts from each piece's live shape (Shape pieces), so what
            // you see on your character is what gets baked.
            g_settings.back().morphs.clear();
            for (const auto& key : it.styles) {
                g_settings.back().morphs.push_back(PieceMorphs::Get(key));
            }
            g_filters.emplace_back(it.styles.size());
            triPaths.push_back(it.triPaths);
            triPaths.back().resize(it.styles.size());
        }
        g_names.clear();
        g_namesReady = false;
        g_namesJob   = std::async(std::launch::async, [paths = std::move(triPaths)] {
            MorphNameTable out;
            for (const auto& item : paths) {
                auto& row = out.emplace_back();
                for (const auto& piece : item) {
                    row.push_back(ArmorExport::MorphNames(piece));
                }
            }
            return out;
        });
        // An outfit name in a non-Latin script sanitizes to nothing; fall back
        // to a name that is still unique per outfit.
        auto readable = M::SafeName(a_outfit.name, 32, "");
        if (readable.empty()) {
            readable = "Outfit " + M::HashHex(a_outfit.name).substr(0, 6);
        }
        g_pluginName = M::SafeName("FR Armor - " + readable, 48, "Fitting Room Armor");
        // A finished status from an earlier export is cleared on reopen.
        if (!ArmorExport::GetStatus().busy) {
            ArmorExport::Acknowledge();
        }
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
        const float em     = OS::ui::FontSize();
        const auto  status = ArmorExport::GetStatus();
        PollNames();

        FUCK::TextUnformatted("Make real armor");
        FUCK::TextDisabled("%s", "Turns the outfit on your character into real items you can equip.");
        FUCK::Separator();

        FUCK::BeginDisabled(status.busy);
        FUCK::SetNextItemWidth(em * 18.0f);
        FUCK::InputText("Plugin name", &g_pluginName);
        if (FUCK::IsItemHovered()) {
            FUCK::SetTooltip("Saved as this name + .esp in your Data folder.\n"
                             "Under Mod Organizer it lands in Overwrite");
        }
        FUCK::Spacing();

        bool anyIncluded = false;
        if (g_plan.items.empty()) {
            FUCK::TextWrapped("%s", "This outfit has no styled pieces to make into armor.");
        }
        static const std::vector<std::string> kTypes{ "Light", "Heavy", "Clothing" };
        for (std::size_t i = 0; i < g_plan.items.size() && i < g_settings.size(); ++i) {
            const auto& plan = g_plan.items[i];
            auto&       s    = g_settings[i];
            FUCK::PushID(static_cast<int>(i));
            FUCK::Separator();
            FUCK::Checkbox(M::BucketLabel(plan.bucket), &s.include, false, false);
            std::string pieces;
            for (const auto& n : plan.pieceNames) {
                pieces += (pieces.empty() ? "" : ", ") + n;
            }
            FUCK::TextDisabled("%s", ("From: " + pieces).c_str());
            if (s.include) {
                anyIncluded = true;
                FUCK::SetNextItemWidth(em * 18.0f);
                FUCK::InputText("Name", &s.name);
                int type = TypeIndex(s.type);
                FUCK::SetNextItemWidth(em * 8.0f);
                if (FUCK::Combo("Type", &type, kTypes) && type >= 0 && type <= 2) {
                    s.type = static_cast<M::ArmorType>(type);
                }
                FUCK::BeginDisabled(s.type == M::ArmorType::kClothing);
                FUCK::SetNextItemWidth(em * 8.0f);
                FUCK::DragFloat("Armor rating", &s.rating, 0.25f, 0.0f, 1000.0f, "%.1f");
                FUCK::EndDisabled();
                FUCK::SetNextItemWidth(em * 8.0f);
                FUCK::DragFloat("Weight", &s.weight, 0.1f, 0.0f, 200.0f, "%.1f");
                FUCK::SetNextItemWidth(em * 8.0f);
                FUCK::DragInt("Value", &s.value, 1.0f, 0, 1000000);
                for (std::size_t p = 0; p < plan.pieceNames.size() && p < s.morphs.size(); ++p) {
                    FUCK::PushID(static_cast<int>(1000 + p));
                    DrawPieceMorphs(i, p, plan.pieceNames[p], s.morphs[p], em);
                    FUCK::PopID();
                }
            }
            FUCK::PopID();
        }
        FUCK::Separator();
        if (!g_plan.skipped.empty()) {
            std::string sk = "Left out: ";
            for (std::size_t i = 0; i < g_plan.skipped.size(); ++i) {
                sk += (i ? "; " : "") + g_plan.skipped[i];
            }
            FUCK::TextWrapped("%s", sk.c_str());
        }
        FUCK::Checkbox("Craftable for free at any forge", &g_recipe, false, false);
        FUCK::EndDisabled();

        FUCK::Spacing();
        if (!status.message.empty()) {
            if (status.finished && !status.ok) {
                FUCK::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s", "Not exported:");
            }
            FUCK::PushTextWrapPos(em * 34.0f);
            FUCK::TextWrapped("%s", status.message.c_str());
            FUCK::PopTextWrapPos();
        }
        FUCK::Spacing();

        FUCK::BeginDisabled(status.busy || !anyIncluded);
        if (ChamferPanel::Button("Create").clicked) {
            ArmorExport::Request req;
            req.pluginName = g_pluginName;
            req.recipe     = g_recipe;
            for (std::size_t i = 0; i < g_plan.items.size() && i < g_settings.size(); ++i) {
                req.items.emplace_back(g_plan.items[i], g_settings[i]);
            }
            ArmorExport::Start(std::move(req));
        }
        FUCK::EndDisabled();
        FUCK::SameLine();
        // Never disabled: a long export (big textures compress slowly) keeps
        // running in the background, and its result is in the log and shown
        // here again when the window is reopened.
        // Disabled only while dyed textures are being copied off the GPU, which
        // needs the editor's render hook (a few frames per texture).
        const char* closeLabel = status.copying ? "Copying textures..."
                                 : status.busy  ? "Close (keeps working)"
                                                : (status.finished ? "Done" : "Cancel");
        FUCK::BeginDisabled(status.copying);
        const bool closeClicked = ChamferPanel::Button(closeLabel).clicked;
        FUCK::EndDisabled();
        if (closeClicked) {
            if (!status.busy) {
                ArmorExport::Acknowledge();
            }
            FUCK::CloseCurrentPopup();
        }
        FUCK::EndPopup();
    }

}  // namespace OS::ArmorExportUI
