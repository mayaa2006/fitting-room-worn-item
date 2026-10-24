#pragma once

// Full read and write of the body-slider morph file (.tri, "PIRT") that
// BodySlide writes beside a built mesh and RaceMenu reads through the mesh's
// BODYTRI link (addon, "Make real armor").
//
// BodyMorphTri.h already reads the NAMES out of these files and skips the
// vertex data by arithmetic; this reads the data too, because the export needs
// it twice: to bake a piece's own morph values into its exported geometry, and
// to write one combined tri for a merged item so the exported armor still
// follows the wearer's body sliders.
//
//   char   magic[4]      "PIRT"
//   u16    shapeCount
//   per shape:
//     u8   nameLen, char name[nameLen]
//     u16  morphCount
//     per morph:
//       u8   nameLen, char name[nameLen]
//       f32  multiplier
//       u16  vertexCount
//       vertexCount * (u16 index, i16 dx, i16 dy, i16 dz)
//
// A vertex moves by (dx, dy, dz) * multiplier * morph value, added to its
// position in the shape's own space - what RaceMenu does at runtime.
//
// The raw integers are kept, not decoded floats, so a file that is only
// re-arranged (shapes renamed and gathered into one file) is written back
// bit for bit. Engine-free: tests/test_trifile.cpp.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <map>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OS::TriFile {

    struct Delta {
        std::uint16_t index{ 0 };
        std::int16_t  x{ 0 };
        std::int16_t  y{ 0 };
        std::int16_t  z{ 0 };
    };

    struct Morph {
        std::string        name;
        float              multiplier{ 0.0f };
        std::vector<Delta> deltas;

        [[nodiscard]] std::array<float, 3> Offset(const Delta& a_d) const {
            return { a_d.x * multiplier, a_d.y * multiplier, a_d.z * multiplier };
        }
    };

    struct Shape {
        std::string        name;
        std::vector<Morph> morphs;
        // Texture-coordinate morphs (BodySlide's UV sliders), from the file's
        // second section; z is unused. Carried through untouched.
        std::vector<Morph> uvMorphs;
    };

    using File = std::vector<Shape>;

    namespace detail {
        struct Reader {
            std::string_view b;
            std::size_t      at{ 0 };
            bool             ok{ true };

            bool Have(std::size_t a_n) const { return ok && a_n <= b.size() - at; }
            std::uint8_t U8() {
                if (!Have(1)) {
                    ok = false;
                    return 0;
                }
                return static_cast<std::uint8_t>(b[at++]);
            }
            std::uint16_t U16() {
                if (!Have(2)) {
                    ok = false;
                    return 0;
                }
                const auto lo = static_cast<std::uint8_t>(b[at]);
                const auto hi = static_cast<std::uint8_t>(b[at + 1]);
                at += 2;
                return static_cast<std::uint16_t>(lo | (hi << 8));
            }
            std::int16_t I16() { return static_cast<std::int16_t>(U16()); }
            float F32() {
                if (!Have(4)) {
                    ok = false;
                    return 0.0f;
                }
                std::uint32_t u = 0;
                for (int i = 3; i >= 0; --i) {
                    u = (u << 8) | static_cast<std::uint8_t>(b[at + static_cast<std::size_t>(i)]);
                }
                at += 4;
                float f;
                std::memcpy(&f, &u, 4);
                return f;
            }
            std::string Str() {
                const auto n = U8();
                if (!Have(n)) {
                    ok = false;
                    return {};
                }
                std::string s{ b.substr(at, n) };
                at += n;
                return s;
            }
        };

        inline void PutU8(std::vector<std::uint8_t>& o, std::uint8_t v) { o.push_back(v); }
        inline void PutU16(std::vector<std::uint8_t>& o, std::uint16_t v) {
            o.push_back(static_cast<std::uint8_t>(v & 0xFF));
            o.push_back(static_cast<std::uint8_t>(v >> 8));
        }
        inline void PutF32(std::vector<std::uint8_t>& o, float f) {
            std::uint32_t u;
            std::memcpy(&u, &f, 4);
            for (int i = 0; i < 4; ++i) {
                o.push_back(static_cast<std::uint8_t>((u >> (8 * i)) & 0xFF));
            }
        }
        inline void PutStr(std::vector<std::uint8_t>& o, std::string_view s) {
            const auto n = s.size() > 255 ? std::size_t{ 255 } : s.size();
            PutU8(o, static_cast<std::uint8_t>(n));
            o.insert(o.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
        }
    }

    // Nothing for a file that is not a tri or is cut short: half a file would
    // move some vertices and not others.
    //
    // ⚠ TWO SECTIONS, AS BODYSLIDE WRITES THEM (TriFile::Write, BodySlide and
    // Outfit Studio source): the position morphs, then a u16 count of shapes
    // with texture-coordinate morphs and those morphs (6 bytes a delta: index,
    // u, v). The second section is optional on read, as BodySlide's own reader
    // treats it; a file that ends cleanly after the first is an older one.
    [[nodiscard]] inline std::optional<File> Parse(std::string_view a_bytes) {
        if (a_bytes.size() < 6 || a_bytes.substr(0, 4) != "PIRT") {
            return std::nullopt;
        }
        detail::Reader r{ a_bytes, 4, true };
        File           out;
        const auto     shapes = r.U16();
        for (std::size_t s = 0; s < shapes && r.ok; ++s) {
            Shape sh;
            sh.name           = r.Str();
            const auto morphs = r.U16();
            for (std::size_t m = 0; m < morphs && r.ok; ++m) {
                Morph mo;
                mo.name          = r.Str();
                mo.multiplier    = r.F32();
                const auto count = r.U16();
                // Every delta is 8 bytes: refuse a count the file cannot hold
                // before reserving for it.
                if (!r.Have(static_cast<std::size_t>(count) * 8u)) {
                    return std::nullopt;
                }
                mo.deltas.resize(count);
                for (auto& d : mo.deltas) {
                    d.index = r.U16();
                    d.x     = r.I16();
                    d.y     = r.I16();
                    d.z     = r.I16();
                }
                sh.morphs.push_back(std::move(mo));
            }
            out.push_back(std::move(sh));
        }
        if (!r.ok) {
            return std::nullopt;
        }
        if (r.at < a_bytes.size()) {
            const auto uvShapes = r.U16();
            for (std::size_t s = 0; s < uvShapes && r.ok; ++s) {
                const auto name   = r.Str();
                const auto morphs = r.U16();
                std::vector<Morph> uv;
                for (std::size_t m = 0; m < morphs && r.ok; ++m) {
                    Morph mo;
                    mo.name          = r.Str();
                    mo.multiplier    = r.F32();
                    const auto count = r.U16();
                    if (!r.Have(static_cast<std::size_t>(count) * 6u)) {
                        return std::nullopt;
                    }
                    mo.deltas.resize(count);
                    for (auto& d : mo.deltas) {
                        d.index = r.U16();
                        d.x     = r.I16();
                        d.y     = r.I16();
                    }
                    uv.push_back(std::move(mo));
                }
                Shape* into = nullptr;
                for (auto& sh : out) {
                    if (sh.name == name) {
                        into = &sh;
                        break;
                    }
                }
                if (!into) {
                    out.push_back(Shape{ name, {}, {} });
                    into = &out.back();
                }
                for (auto& m : uv) {
                    into->uvMorphs.push_back(std::move(m));
                }
            }
            if (!r.ok) {
                return std::nullopt;
            }
        }
        return out;
    }

    [[nodiscard]] inline std::optional<File> Parse(const std::vector<std::uint8_t>& a_bytes) {
        return Parse(std::string_view(reinterpret_cast<const char*>(a_bytes.data()), a_bytes.size()));
    }

    // At most 65535 shapes, morphs per shape and deltas per morph are written
    // (the wire fields are u16); a well-formed input never has more. Shapes
    // with no morphs of a kind are left out of that kind's section, and the
    // second section's count is always written, both as BodySlide does.
    [[nodiscard]] inline std::vector<std::uint8_t> Serialize(const File& a_file) {
        std::vector<std::uint8_t> o{ 'P', 'I', 'R', 'T' };
        const auto clamp = [](std::size_t n) { return static_cast<std::uint16_t>(n > 0xFFFF ? 0xFFFF : n); };
        const auto section = [&](bool a_uv) {
            std::vector<const Shape*> list;
            for (const auto& sh : a_file) {
                if (!(a_uv ? sh.uvMorphs : sh.morphs).empty() && list.size() < 0xFFFF) {
                    list.push_back(&sh);
                }
            }
            detail::PutU16(o, static_cast<std::uint16_t>(list.size()));
            for (const auto* sh : list) {
                const auto& morphsOf = a_uv ? sh->uvMorphs : sh->morphs;
                detail::PutStr(o, sh->name);
                const auto morphs = clamp(morphsOf.size());
                detail::PutU16(o, morphs);
                for (std::size_t m = 0; m < morphs; ++m) {
                    const auto& mo = morphsOf[m];
                    detail::PutStr(o, mo.name);
                    detail::PutF32(o, mo.multiplier);
                    const auto count = clamp(mo.deltas.size());
                    detail::PutU16(o, count);
                    for (std::size_t i = 0; i < count; ++i) {
                        const auto& d = mo.deltas[i];
                        detail::PutU16(o, d.index);
                        detail::PutU16(o, static_cast<std::uint16_t>(d.x));
                        detail::PutU16(o, static_cast<std::uint16_t>(d.y));
                        if (!a_uv) {
                            detail::PutU16(o, static_cast<std::uint16_t>(d.z));
                        }
                    }
                }
            }
        };
        section(false);
        section(true);
        return o;
    }

    // One morph from float offsets, quantized the way BodySlide's writer does
    // (multiplier = largest component / 32767, components truncated). Zero
    // offsets are dropped. Empty when nothing moves.
    [[nodiscard]] inline Morph Quantize(std::string a_name, const std::vector<std::uint16_t>& a_indices,
                                        const std::vector<std::array<float, 3>>& a_offsets, bool a_uv = false) {
        Morph m;
        m.name    = std::move(a_name);
        float big = 0.0f;
        for (std::size_t i = 0; i < a_indices.size() && i < a_offsets.size(); ++i) {
            for (int c = 0; c < (a_uv ? 2 : 3); ++c) {
                const float v = a_offsets[i][static_cast<std::size_t>(c)];
                big           = std::max(big, v < 0.0f ? -v : v);
            }
        }
        if (big <= 0.0f) {
            return m;
        }
        m.multiplier = big / 32767.0f;
        for (std::size_t i = 0; i < a_indices.size() && i < a_offsets.size(); ++i) {
            const auto& v = a_offsets[i];
            if (v[0] == 0.0f && v[1] == 0.0f && (a_uv || v[2] == 0.0f)) {
                continue;
            }
            Delta d;
            d.index = a_indices[i];
            d.x     = static_cast<std::int16_t>(v[0] / m.multiplier);
            d.y     = static_cast<std::int16_t>(v[1] / m.multiplier);
            d.z     = a_uv ? std::int16_t{ 0 } : static_cast<std::int16_t>(v[2] / m.multiplier);
            m.deltas.push_back(d);
        }
        return m;
    }

    // The shapes of a_file that a merge kept, under their merged names.
    // a_names maps original shape names to merged ones; a tri names shapes the
    // way the engine compares them, so an exact match wins and otherwise a
    // case-blind one counts when it is unique. Shapes with no match are left
    // out (their geometry is not in the merged file).
    [[nodiscard]] inline File Renamed(const File& a_file, const std::map<std::string, std::string>& a_names) {
        const auto lower = [](std::string x) {
            for (auto& c : x) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return x;
        };
        File out;
        for (const auto& sh : a_file) {
            const std::string* to = nullptr;
            if (const auto it = a_names.find(sh.name); it != a_names.end()) {
                to = &it->second;
            } else {
                const auto want = lower(sh.name);
                int        hits = 0;
                for (const auto& [from, merged] : a_names) {
                    if (lower(from) == want) {
                        to = &merged;
                        ++hits;
                    }
                }
                if (hits != 1) {
                    to = nullptr;
                }
            }
            if (!to) {
                continue;
            }
            auto copy = sh;
            copy.name = *to;
            out.push_back(std::move(copy));
        }
        return out;
    }

}  // namespace OS::TriFile
