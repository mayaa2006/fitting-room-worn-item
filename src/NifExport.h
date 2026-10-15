#pragma once

// "Make real armor" export (addon): rewrites a copy of an armor NIF so its
// shapes point at the exported (dye-baked) textures. Built on nifly, the
// GPL-3.0 NIF library Outfit Studio uses. Engine-free: takes bytes, returns
// bytes, so it is unit-tested off-game (tests/test_nifexport.cpp).

#include "TriFile.h"

#include <array>
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
        // Leave this shape out of the file (a part hidden in the outfit).
        bool  remove{ false };
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
        int                       removed{ 0 };    // shapes left out (ShapeEdit::remove)
        int                       droppedTri{ 0 };      // Merge: non-base files with a BODYTRI link
        int                       droppedPhysics{ 0 };  // Merge: non-base files with HDT/SMP physics
        // Merge: per input file, each shape's original name -> its name in
        // the merged file (renamed when the name was taken). The base file's
        // names map to themselves.
        std::vector<std::map<std::string, std::string>> names;
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

    // The meshes-relative path of the file's body-slider morph file (the
    // BODYTRI string on the root, or on any shape), or empty.
    [[nodiscard]] std::string BodyTriPath(const std::vector<std::uint8_t>& a_nif);

    // Bakes body-slider morphs into the geometry: every shape whose name
    // matches a tri shape (exactly, else case-blind when that is unique) moves
    // each vertex by the sum over a_values of value * that morph's offset -
    // what RaceMenu would do with those values, frozen into the file.
    // Out-of-range vertex indices are ignored. Result::edited counts shapes
    // moved.
    [[nodiscard]] Result ApplyMorphs(const std::vector<std::uint8_t>&    a_nif,
                                     const TriFile::File&                a_tri,
                                     const std::map<std::string, float>& a_values);

    // Replaces every BODYTRI link in the file (root and shapes) with one on
    // the root naming a_path (meshes-relative). Empty a_path only removes.
    [[nodiscard]] Result SetBodyTri(const std::vector<std::uint8_t>& a_nif, const std::string& a_path);

    // Shape names in the same depth-first order Rewrite matches by. For tests
    // and diagnostics.
    [[nodiscard]] std::vector<std::string> ShapeNames(const std::vector<std::uint8_t>& a_nif);

    // Vertex positions of the shape at a_order (depth-first), or empty. For
    // tests and diagnostics.
    [[nodiscard]] std::vector<std::array<float, 3>> Vertices(const std::vector<std::uint8_t>& a_nif, int a_order);

    // Texture-set paths of the shape at a_order (depth-first), or empty.
    [[nodiscard]] std::vector<std::string> TexturePaths(const std::vector<std::uint8_t>& a_nif,
                                                        int a_order);

}  // namespace OS::NifExport
