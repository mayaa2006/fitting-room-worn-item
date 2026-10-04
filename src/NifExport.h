#pragma once

// "Make real armor" export (addon): rewrites a copy of an armor NIF so its
// shapes point at the exported (dye-baked) textures. Built on nifly, the
// GPL-3.0 NIF library Outfit Studio uses. Engine-free: takes bytes, returns
// bytes, so it is unit-tested off-game (tests/test_nifexport.cpp).

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace OS::NifExport {

    // What to change on one shape. A shape is found by NAME when exactly one
    // shape in the file and exactly one edit carry that name, otherwise by its
    // depth-first ORDER among the file's shapes, which is the order the engine
    // walks a loaded model in.
    struct ShapeEdit {
        std::string name;
        int         order{ -1 };
        // Texture-set slot (0-8) -> path to write. Paths are written as given,
        // so pass NIF form ("textures\...").
        std::map<std::uint32_t, std::string> textures;
        bool  hasSpecular{ false };
        float specular[3]{ 1.0f, 1.0f, 1.0f };
        float glossiness{ 0.0f };
        float specularStrength{ 1.0f };
    };

    struct Result {
        bool                      ok{ false };
        std::vector<std::uint8_t> bytes;
        int                       shapes{ 0 };     // shapes in the file
        int                       edited{ 0 };     // shapes an edit landed on
        int                       remapped{ 0 };   // texture slots changed by a_remap
        int                       unmatched{ 0 };  // edits that found no shape
        int                       droppedExtra{ 0 };  // Merge: non-base files whose root extra data
                                                      // (BODYTRI morphs, physics) could not come along
        std::string               error;
    };

    // Applies a_edits, then a_remap to EVERY shape's texture set: any slot
    // whose path has the same ArmorExportModel::TextureKey as a remap key is
    // replaced with the remap value. The remap is how the first-person model,
    // which has no live counterpart to match, still picks up the baked dyes.
    // A texture set or shader shared between shapes is cloned before it is
    // changed, so an edit never leaks onto a shape it was not meant for.
    [[nodiscard]] Result Rewrite(const std::vector<std::uint8_t>&          a_nif,
                                 const std::vector<ShapeEdit>&             a_edits,
                                 const std::map<std::string, std::string>& a_remap);

    // Merges several (already rewritten) armor NIFs into one: the first is the
    // base, every shape of the others is cloned in with its skin, partitions,
    // shader and textures, and any bone the base lacks is cloned across too
    // (nifly's CloneShape, the path Outfit Studio's merge takes). A shape whose
    // name is already taken gets a numbered suffix. One input returns it
    // unchanged.
    [[nodiscard]] Result Merge(const std::vector<std::vector<std::uint8_t>>& a_nifs);

    // Self-contained export: every texture path the file's ARMOUR shapes use
    // (lighting texture sets and effect-shader textures). Skin shapes - face,
    // skin-tint and hair-tint lighting shaders - are left out on purpose: the
    // engine paints those with the wearer's own skin at runtime, so a copied
    // path there would be ignored at best and freeze one skin at worst.
    [[nodiscard]] std::vector<std::string> LocalizableTextures(const std::vector<std::uint8_t>& a_nif);

    // Points those same armour-shape paths at new files: any path whose
    // ArmorExportModel::TextureKey is a key of a_map becomes its value. Skin
    // shapes are never touched, and a texture set shared with one is cloned
    // first. Result::remapped counts the slots changed.
    [[nodiscard]] Result Localize(const std::vector<std::uint8_t>&          a_nif,
                                  const std::map<std::string, std::string>& a_map);

    // Shape names in the same depth-first order Rewrite matches by. For tests
    // and diagnostics.
    [[nodiscard]] std::vector<std::string> ShapeNames(const std::vector<std::uint8_t>& a_nif);

    // Texture-set paths of the shape at a_order (depth-first), or empty.
    [[nodiscard]] std::vector<std::string> TexturePaths(const std::vector<std::uint8_t>& a_nif,
                                                        int a_order);

}  // namespace OS::NifExport
