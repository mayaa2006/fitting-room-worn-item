// Tests for the "Make real armor" NIF rewriter and merger (addon), run against
// nifly's own Skyrim SE sample meshes (NIFLY_TEST_INPUT, set by CMake).
#include "ArmorExportModel.h"
#include "NifExport.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

#ifndef NIFLY_TEST_INPUT
#    error "NIFLY_TEST_INPUT must point at nifly/tests/input"
#endif

namespace {
    std::vector<std::uint8_t> ReadFile(const std::string& a_name) {
        std::ifstream in(std::string(NIFLY_TEST_INPUT) + "/" + a_name, std::ios::binary);
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
    }
}

int main() {
    using namespace OS::NifExport;
    namespace M = OS::ArmorExportModel;

    const auto skinned = ReadFile("TestNifFile_Skinned_SE.nif");
    const auto dynamic = ReadFile("TestNifFile_Skinned_Dynamic_SE.nif");
    CHECK(!skinned.empty());
    CHECK(!dynamic.empty());
    if (skinned.empty() || dynamic.empty()) {
        std::printf("sample NIFs missing under %s\n", NIFLY_TEST_INPUT);
        return 1;
    }

    const auto names = ShapeNames(skinned);
    CHECK(!names.empty());
    std::printf("skinned sample: %zu shape(s)\n", names.size());

    // Find a shape that has a texture set to work on.
    int target = -1;
    std::vector<std::string> before;
    for (int i = 0; i < static_cast<int>(names.size()); ++i) {
        before = TexturePaths(skinned, i);
        if (!before.empty()) {
            target = i;
            break;
        }
    }
    CHECK(target >= 0);
    if (target < 0) {
        return 1;
    }

    {  // An edit by ORDER (name blank) lands on that shape and only there.
        ShapeEdit e;
        e.order       = target;
        e.textures[0] = "textures\\FittingRoom\\Armor\\Test\\baked_d.dds";
        e.hasSpecular = true;
        e.specular[0] = 0.5f;
        e.glossiness  = 42.0f;
        const auto r = Rewrite(skinned, { e }, {});
        CHECK(r.ok);
        CHECK(r.edited == 1);
        CHECK(r.unmatched == 0);
        CHECK(r.shapes == static_cast<int>(names.size()));
        const auto after = TexturePaths(r.bytes, target);
        CHECK(!after.empty() && after[0] == "textures\\FittingRoom\\Armor\\Test\\baked_d.dds");
        // Other slots untouched.
        for (std::size_t s = 1; s < after.size() && s < before.size(); ++s) {
            CHECK(after[s] == before[s]);
        }
        // Every other shape keeps its diffuse.
        for (int i = 0; i < static_cast<int>(names.size()); ++i) {
            if (i == target) {
                continue;
            }
            const auto o = TexturePaths(skinned, i);
            const auto n = TexturePaths(r.bytes, i);
            CHECK(o == n);
        }
        CHECK(ShapeNames(r.bytes) == names);
    }

    {  // An edit by NAME, and an edit that matches nothing is counted.
        ShapeEdit e;
        e.name        = names[static_cast<std::size_t>(target)];
        e.order       = 9999;
        e.textures[1] = "textures\\x_n.dds";
        ShapeEdit ghost;
        ghost.name  = "NoSuchShape";
        ghost.order = 9999;
        const auto r = Rewrite(skinned, { e, ghost }, {});
        CHECK(r.ok);
        CHECK(r.edited == 1);
        CHECK(r.unmatched == 1);
        const auto after = TexturePaths(r.bytes, target);
        CHECK(after.size() > 1 && after[1] == "textures\\x_n.dds");
    }

    {  // A name the file does not have is runtime geometry (a RaceMenu
       // overlay, say): it must NOT fall back to its position.
        ShapeEdit e;
        e.name        = "[Ovl0]";
        e.order       = target;
        e.textures[0] = "textures\\wrong.dds";
        const auto r = Rewrite(skinned, { e }, {});
        CHECK(r.ok);
        CHECK(r.edited == 0);
        CHECK(r.unmatched == 1);
        CHECK(TexturePaths(r.bytes, target) == before);
    }

    {  // The remap rewrites any slot whose path matches, whatever its case,
       // slashes or "textures\" prefix.
        std::string orig;
        for (const auto& t : before) {
            if (!t.empty()) {
                orig = t;
                break;
            }
        }
        CHECK(!orig.empty());
        std::string fancy = orig;
        for (auto& c : fancy) {
            if (c == '\\') c = '/';
        }
        const auto key = M::TextureKey(fancy);
        CHECK(key == M::TextureKey(orig));
        const auto r = Rewrite(skinned, {}, { { key, "textures\\remapped.dds" } });
        CHECK(r.ok);
        CHECK(r.remapped >= 1);
        bool found = false;
        for (const auto& t : TexturePaths(r.bytes, target)) {
            found = found || t == "textures\\remapped.dds";
        }
        CHECK(found);
    }

    {  // Merge: every shape of both files survives, duplicates renamed, and
       // the result reloads.
        const auto a = ShapeNames(skinned);
        const auto b = ShapeNames(dynamic);
        const auto m = Merge({ skinned, dynamic });
        CHECK(m.ok);
        CHECK(m.shapes == static_cast<int>(a.size() + b.size()));
        const auto mn = ShapeNames(m.bytes);
        CHECK(mn.size() == a.size() + b.size());
        // The base file's shapes come first, in order.
        for (std::size_t i = 0; i < a.size() && i < mn.size(); ++i) {
            CHECK(mn[i] == a[i]);
        }
        // No two shapes share a name.
        for (std::size_t i = 0; i < mn.size(); ++i) {
            for (std::size_t j = i + 1; j < mn.size(); ++j) {
                CHECK(mn[i] != mn[j]);
            }
        }
        // Merging a file with itself doubles it and renames the copies.
        const auto self = Merge({ skinned, skinned });
        CHECK(self.ok);
        CHECK(self.shapes == static_cast<int>(2 * a.size()));
        // A merged, rewritten file still carries the edit made before merging.
        ShapeEdit e;
        e.order       = target;
        e.textures[0] = "textures\\merged_d.dds";
        const auto edited = Rewrite(skinned, { e }, {});
        const auto m2     = Merge({ dynamic, edited.bytes });
        CHECK(m2.ok);
        bool carried = false;
        for (int i = 0; i < m2.shapes; ++i) {
            const auto t = TexturePaths(m2.bytes, i);
            carried = carried || (!t.empty() && t[0] == "textures\\merged_d.dds");
        }
        CHECK(carried);
        // One input passes through untouched.
        const auto one = Merge({ skinned });
        CHECK(one.ok && one.bytes == skinned);
    }

    {  // Self-contained export: list the armour textures, repoint them all.
        const auto list = LocalizableTextures(skinned);
        CHECK(!list.empty());
        std::map<std::string, std::string> map;
        for (const auto& p : list) {
            map.emplace(M::TextureKey(p), "textures\\FittingRoom\\Armor\\T\\src\\" + M::TextureKey(p));
        }
        const auto r = Localize(skinned, map);
        CHECK(r.ok);
        CHECK(r.remapped >= static_cast<int>(list.size()));
        // Every listed texture now lives under the export folder.
        for (const auto& p : LocalizableTextures(r.bytes)) {
            CHECK(M::TextureKey(p).starts_with("fittingroom\\armor\\t\\src\\"));
        }
        CHECK(ShapeNames(r.bytes) == names);
        // Nothing to map is a no-op that still round-trips.
        const auto none = Localize(skinned, {});
        CHECK(none.ok && none.remapped == 0);
    }

    {  // Garbage in is an error, not a crash.
        const auto r = Rewrite({ 1, 2, 3, 4 }, {}, {});
        CHECK(!r.ok);
        CHECK(!r.error.empty());
        const auto m = Merge({ skinned, { 9, 9, 9 } });
        CHECK(!m.ok);
    }

    if (g_failures == 0) {
        std::printf("all NifExport tests passed\n");
        return 0;
    }
    std::printf("%d NifExport check(s) failed\n", g_failures);
    return 1;
}
