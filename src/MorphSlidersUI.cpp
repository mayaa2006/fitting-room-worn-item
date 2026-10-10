#include "MorphSlidersUI.h"

#include "FuckCompat.h"

#include <cctype>

namespace OS::MorphSlidersUI {

    namespace {
        bool Contains(const std::string& a_hay, const std::string& a_needle) {
            if (a_needle.empty()) {
                return true;
            }
            const auto lower = [](std::string x) {
                for (auto& c : x) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                return x;
            };
            return lower(a_hay).find(lower(a_needle)) != std::string::npos;
        }
    }

    std::size_t Changed(const std::map<std::string, float>& a_values) {
        std::size_t n = 0;
        for (const auto& [name, v] : a_values) {
            n += v != 0.0f ? 1 : 0;
        }
        return n;
    }

    bool Draw(const std::vector<std::string>& a_names, std::map<std::string, float>& a_values,
              std::string& a_filter, float a_em) {
        bool changed = false;
        FUCK::SetNextItemWidth(a_em * 12.0f);
        FUCK::InputText("Find", &a_filter);
        FUCK::SameLine();
        FUCK::BeginDisabled(Changed(a_values) == 0);
        if (FUCK::Button("Reset")) {
            a_values.clear();
            changed = true;
        }
        FUCK::EndDisabled();
        FUCK::BeginChild("morph_rows", ImVec2(0, a_em * 12.0f), true);
        for (const auto& n : a_names) {
            if (!Contains(n, a_filter)) {
                continue;
            }
            const auto it  = a_values.find(n);
            float      pct = it == a_values.end() ? 0.0f : it->second * 100.0f;
            FUCK::SetNextItemWidth(a_em * 10.0f);
            if (FUCK::DragFloat(n.c_str(), &pct, 1.0f, -200.0f, 200.0f, "%.0f%%")) {
                if (pct == 0.0f) {
                    a_values.erase(n);
                } else {
                    a_values[n] = pct / 100.0f;
                }
                changed = true;
            }
            if (FUCK::IsItemHovered()) {
                FUCK::SetTooltip("Drag to change. Zero leaves the piece as it was built.");
            }
        }
        FUCK::EndChild();
        return changed;
    }

}  // namespace OS::MorphSlidersUI
