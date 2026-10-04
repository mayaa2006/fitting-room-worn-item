// Pure-logic tests for the "Make real armor" export helpers (addon).
#include "ArmorExportModel.h"

#include <algorithm>
#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

int main() {
    using namespace OS::ArmorExportModel;

    {  // Texture keys ignore case, slashes, data\ and textures\ prefixes.
        CHECK(TextureKey("Textures/Armor/Iron/Cuirass.dds") == "armor\\iron\\cuirass.dds");
        CHECK(TextureKey("armor\\iron\\cuirass.dds") == "armor\\iron\\cuirass.dds");
        CHECK(TextureKey("Data\\textures\\armor\\iron\\cuirass.dds") == "armor\\iron\\cuirass.dds");
        CHECK(TextureKey("textures\\\\armor//x.dds") == "armor\\x.dds");
        CHECK(TextureKey("").empty());
        CHECK(NifTexturePath("armor/x.dds") == "textures\\armor\\x.dds");
        CHECK(NifTexturePath("Textures\\Armor\\X.dds") == "Textures\\Armor\\X.dds");
        CHECK(NifTexturePath("").empty());
    }
    {  // Model paths.
        CHECK(RecordModelPath("meshes\\Armor\\X_1.nif") == "Armor\\X_1.nif");
        CHECK(RecordModelPath("Armor/X_1.nif") == "Armor\\X_1.nif");
        CHECK(ArchiveMeshPath("Armor\\X_1.nif") == "meshes\\Armor\\X_1.nif");
        const auto v = MeshVariants("Armor\\Iron\\F\\Cuirass_1.nif");
        CHECK(v.size() == 2);
        CHECK(v[0].suffix == "_0" && v[0].path == "Armor\\Iron\\F\\Cuirass_0.nif");
        CHECK(v[1].suffix == "_1" && v[1].path == "Armor\\Iron\\F\\Cuirass_1.nif");
        const auto u = MeshVariants("Armor\\Iron\\F\\cuirass_0.NIF");
        CHECK(u.size() == 2 && u[1].path == "Armor\\Iron\\F\\cuirass_1.nif");
        const auto s = MeshVariants("Clothes\\Ring.nif");
        CHECK(s.size() == 1 && s[0].suffix.empty() && s[0].path == "Clothes\\Ring.nif");
    }
    {  // Dye texture names.
        CHECK(IsDyeTextureName("textures\\armor\\x.dds|FF0000|m|c"));
        CHECK(!IsDyeTextureName("textures\\armor\\x.dds"));
        CHECK(DyeIdentity("textures\\armor\\x.dds|FF0000|c") == "textures\\armor\\x.dds");
        CHECK(DyeIdentity("plain").empty());
        CHECK(IsDyePreviewName("x|FF0000|p"));
        CHECK(!IsDyeTextureName("00ABCDEF|NPC Head"));  // an OverlayBake scratch name
        CHECK(IsInjectedOverlayName("[Ovl3]") && IsInjectedOverlayName("[SOvl0]"));
        CHECK(!IsInjectedOverlayName("Cuirass"));
        CHECK(!IsDyePreviewName("x|FF0000|c"));
    }
    {  // Names.
        CHECK(SafeName("  My: Outfit?! ", 40, "X") == "My Outfit");
        CHECK(SafeName("???", 40, "Fallback") == "Fallback");
        CHECK(SafeName("abcdefghij", 4, "X") == "abcd");
        CHECK(SafeStem("My Outfit", 40, "X") == "My_Outfit");
        CHECK(EditorIdPart("Iron & Steel 2", 32) == "IronSteel2");
        CHECK(EditorIdPart("!!", 32) == "Piece");
        CHECK(PluginStem("My Armor.esp") == "My Armor");
        CHECK(PluginStem("My Armor.ESL ") == "My Armor");
        CHECK(PluginStem("My Armor") == "My Armor");
        CHECK(HashHex("abc") == HashHex("abc") && HashHex("abc") != HashHex("abd"));
        CHECK(HashHex("").size() == 16);
        const auto cf = CopiedTextureFileName("armor\\some\\very\\deep\\folder\\IronCuirass_n.dds");
        CHECK(cf.size() < 64);
        CHECK(cf.ends_with("_IronCuirass_n.dds"));
        CHECK(CopiedTextureFileName("a\\x.dds") != CopiedTextureFileName("b\\x.dds"));
    }
    {  // The four items.
        CHECK(BucketFor(kBodyBit) == Bucket::kTorso);
        CHECK(BucketFor(kBodyBit | kHandsBit) == Bucket::kTorso);   // body wins
        CHECK(BucketFor(kBodyBit | kHeadBits) == Bucket::kTorso);   // a robe with a hood
        CHECK(BucketFor(kHandsBit) == Bucket::kGloves);
        CHECK(BucketFor(kFeetBit) == Bucket::kBoots);
        CHECK(BucketFor(1u << 1) == Bucket::kHelmet);               // hair slot 31
        CHECK(BucketFor(kCircletBit) == Bucket::kHelmet);
        CHECK(BucketFor(1u << 16) == Bucket::kTorso);               // slot 46, a cloak
        CHECK(BucketFor(kAmuletBit) == Bucket::kTorso);             // jewelry joins the torso
        CHECK(BucketFor(kShieldBit) == Bucket::kSkip);
        CHECK(BucketFor(0) == Bucket::kTorso);
    }
    {  // Keywords.
        auto has = [](const std::vector<std::string>& v, const char* k) {
            return std::find(v.begin(), v.end(), k) != v.end();
        };
        const auto heavy = KeywordEditorIds(kBodyBit, ArmorType::kHeavy);
        CHECK(has(heavy, "ArmorHeavy") && has(heavy, "ArmorCuirass") && has(heavy, "VendorItemArmor"));
        CHECK(!has(heavy, "ArmorLight"));
        const auto cloth = KeywordEditorIds(kFeetBit, ArmorType::kClothing);
        CHECK(has(cloth, "ArmorClothing") && has(cloth, "ClothingFeet") && has(cloth, "VendorItemClothing"));
        const auto helm = KeywordEditorIds(kHeadBits, ArmorType::kLight);
        CHECK(has(helm, "ArmorLight") && has(helm, "ArmorHelmet"));
        const auto torsoPlus = KeywordEditorIds(kBodyBit | (1u << 16), ArmorType::kLight);
        CHECK(has(torsoPlus, "ArmorCuirass"));
    }

    if (g_failures == 0) {
        std::printf("all ArmorExportModel tests passed\n");
        return 0;
    }
    std::printf("%d ArmorExportModel check(s) failed\n", g_failures);
    return 1;
}
