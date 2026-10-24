#pragma once

// "Make real armor" at zeroed sliders (addon): the engine-free arithmetic.
//
// A BodySlide build is the project's reference mesh (ShapeData) plus, per
// slider, value x its .osd deltas - with an inverted slider applied at
// (1 - value). So the build every slider at 0 would give is the reference
// mesh plus the FULL deltas of every inverted slider, and nothing else.
// Zap sliders delete geometry rather than move it and are off at zero (an
// inverted zap would be on; those are reported, never applied). UV sliders
// move texture coordinates and are not part of the shape.
//
// The body-slider file is rebuilt from the same project the way BodySlide's
// build writes it (BodySlideApp::WriteMorphTRI): every non-zap slider's
// deltas at 1.0, NOT inverted, position sliders in the first section and UV
// sliders in the second, indexed on the reference mesh's own vertices.
//
// Tested in tests/test_zeroedmodel.cpp.

#include "BodyMorphData.h"
#include "BodyMorphSource.h"
#include "TriFile.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace OS::ZeroedModel {

    // The NIF shape a project shape is built on.
    [[nodiscard]] inline const std::string& NifShapeName(const BodyMorphSource::ShapeRules& a_s) {
        return a_s.nifName.empty() ? a_s.shape : a_s.nifName;
    }

    // Looks a shape up by exact name, else case-blind when that is unique.
    template <class Map>
    [[nodiscard]] inline auto FindShape(const Map& a_map, const std::string& a_name) -> decltype(a_map.find(a_name)) {
        if (auto it = a_map.find(a_name); it != a_map.end()) {
            return it;
        }
        const auto lower = [](std::string x) {
            for (auto& c : x) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return x;
        };
        const auto want  = lower(a_name);
        auto       found = a_map.end();
        for (auto it = a_map.begin(); it != a_map.end(); ++it) {
            if (lower(it->first) == want) {
                if (found != a_map.end()) {
                    return a_map.end();  // ambiguous
                }
                found = it;
            }
        }
        return found;
    }

    // Per NIF shape: what to add to every reference vertex for the all-zero
    // build. a_vertexCounts is the reference mesh's own count per shape name;
    // a shape the mesh does not have gets nothing.
    [[nodiscard]] inline std::map<std::string, std::vector<std::array<float, 3>>> ZeroOffsets(
        const BodyMorphSource::Source& a_src, const std::map<std::string, std::size_t>& a_vertexCounts) {
        std::map<std::string, std::vector<std::array<float, 3>>> out;
        for (const auto& s : a_src.shapes) {
            const auto& name  = NifShapeName(s);
            const auto  count = FindShape(a_vertexCounts, name);
            if (count == a_vertexCounts.end()) {
                continue;
            }
            BodyMorphData::SliderValues zero;
            for (const auto& [slider, rule] : s.rules) {
                zero.emplace(slider, 0.0f);
            }
            auto field = BodyMorphData::Accumulate(a_src.deltas, s.rules, zero, count->second);
            bool moves = false;
            for (const auto& v : field) {
                moves = moves || v[0] != 0.0f || v[1] != 0.0f || v[2] != 0.0f;
            }
            if (moves) {
                out.emplace(count->first, std::move(field));
            }
        }
        return out;
    }

    // Sliders that would delete geometry at zero (inverted zaps). Not applied.
    [[nodiscard]] inline std::vector<std::string> ZapsOnAtZero(const BodyMorphSource::Source& a_src) {
        std::vector<std::string> out;
        for (const auto& s : a_src.shapes) {
            for (const auto& [slider, rule] : s.rules) {
                if (rule.zap && rule.invert && std::find(out.begin(), out.end(), slider) == out.end()) {
                    out.push_back(slider);
                }
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    // The body-slider file for the zero build, shapes named as in the NIF.
    [[nodiscard]] inline TriFile::File TriFromSource(const BodyMorphSource::Source&          a_src,
                                                     const std::map<std::string, std::size_t>& a_vertexCounts) {
        TriFile::File out;
        for (const auto& s : a_src.shapes) {
            const auto count = FindShape(a_vertexCounts, NifShapeName(s));
            if (count == a_vertexCounts.end()) {
                continue;
            }
            TriFile::Shape shape;
            shape.name = count->first;
            std::vector<std::string> names;
            for (const auto& [slider, rule] : s.rules) {
                names.push_back(slider);
            }
            std::sort(names.begin(), names.end());  // a stable file, whatever the map's order
            for (const auto& slider : names) {
                const auto& rule = s.rules.at(slider);
                if (rule.zap || rule.dataName.empty()) {
                    continue;
                }
                const auto set = a_src.deltas.find(rule.dataName);
                if (set == a_src.deltas.end()) {
                    continue;
                }
                std::vector<std::uint16_t>        idx;
                std::vector<std::array<float, 3>> off;
                for (std::size_t d = 0; d < set->second.indices.size() && d < set->second.deltas.size(); ++d) {
                    if (set->second.indices[d] < count->second) {
                        idx.push_back(set->second.indices[d]);
                        off.push_back(set->second.deltas[d]);
                    }
                }
                auto m = TriFile::Quantize(slider, idx, off, rule.uv);
                if (m.deltas.empty()) {
                    continue;
                }
                (rule.uv ? shape.uvMorphs : shape.morphs).push_back(std::move(m));
            }
            if (!shape.morphs.empty() || !shape.uvMorphs.empty()) {
                out.push_back(std::move(shape));
            }
        }
        return out;
    }

}  // namespace OS::ZeroedModel
