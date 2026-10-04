#include "ArmorExportUI.h"

#include "ArmorExport.h"
#include "ChamferPanel.h"
#include "FuckCompat.h"

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

        int TypeIndex(M::ArmorType a_t) { return static_cast<int>(a_t); }
    }

    void Open(const Outfit& a_outfit) {
        g_plan = ArmorExport::PlanFor(a_outfit, a_outfit.name);
        g_settings.clear();
        for (const auto& it : g_plan.items) {
            g_settings.push_back(it.defaults);
        }
        g_pluginName = M::SafeName("FR Armor - " + a_outfit.name, 48, "Fitting Room Armor");
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
        FUCK::BeginDisabled(status.busy);
        if (ChamferPanel::Button(status.finished ? "Done" : "Cancel").clicked) {
            ArmorExport::Acknowledge();
            FUCK::CloseCurrentPopup();
        }
        FUCK::EndDisabled();
        FUCK::EndPopup();
    }

}  // namespace OS::ArmorExportUI
