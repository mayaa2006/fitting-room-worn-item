#pragma once

// Minimal Skyrim SE plugin writer for the "Make real armor" export (addon).
// Pure: no engine types, so the byte layout is unit-tested off-game
// (tests/test_espwriter.cpp parses what this writes back).
//
// It writes exactly three record types - ARMO, ARMA and COBJ - into a
// non-localized plugin (TES4 flag 0x80 clear, so FULL/DESC are plain zstrings),
// optionally ESL-flagged (0x200). Own records use object ids from 0x800 up,
// which is the range every ESL-capable runtime accepts.
//
// Layout references (UESP "Skyrim Mod:Mod File Format"): every record and group
// header is 24 bytes; a subrecord is a 4-char type, a uint16 size and the data.
// Record header: type, dataSize, flags, formID, timestamp(u16), vc(u16),
// formVersion(u16 = 44 for SSE), unknown(u16). Group header: "GRUP",
// groupSize (header included), label (the record type), groupType 0,
// timestamp(u16), vc(u16), unknown(u32).
//
// FormIDs inside a plugin carry a MASTER INDEX in the top byte - the
// position of the defining file in this plugin's own master list - and the
// plugin's own records use index == master count. Light (ESL) masters are
// indexed the same way; their object id is simply small.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace OS::EspWriter {

    // A form reference: a record in some master (file + object id), or one of
    // this plugin's own records (own == true, `local` is its object id).
    struct FormRef {
        std::string   file;
        std::uint32_t local{ 0 };
        bool          own{ false };

        [[nodiscard]] static FormRef Own(std::uint32_t a_id) { return FormRef{ {}, a_id, true }; }
        [[nodiscard]] bool           Empty() const { return !own && file.empty(); }
    };

    struct Arma {
        std::uint32_t        id{ 0 };
        std::string          edid;
        std::uint32_t        slots{ 0 };      // biped slot bitmask (bit 0 = slot 30)
        std::uint32_t        armorType{ 0 };  // 0 light, 1 heavy, 2 clothing
        FormRef              race;
        std::vector<FormRef> additionalRaces;
        std::int8_t          priority[2]{ 0, 0 };
        std::int8_t          weightSlider[2]{ 0, 0 };  // 0x02 = enabled (_0/_1 pair)
        std::int8_t          detectionSoundValue{ 0 };
        float                weaponAdjust{ 0.0f };
        std::string          model[2];        // male, female (relative to meshes\)
        std::string          model1st[2];     // male, female first person
    };

    struct Armo {
        std::uint32_t              id{ 0 };
        std::string                edid;
        std::string                name;
        std::string                worldModel[2];  // inventory/ground models, may be empty
        std::uint32_t              slots{ 0 };
        std::uint32_t              armorType{ 0 };  // 0 light, 1 heavy, 2 clothing
        FormRef                    race;            // usually Skyrim.esm DefaultRace
        std::vector<FormRef>       keywords;
        std::vector<std::uint32_t> addons;          // own ARMA object ids
        std::int32_t               value{ 0 };
        float                      weight{ 0.0f };
        float                      rating{ 0.0f };  // as shown in game; stored x100
    };

    struct CobjItem {
        FormRef      item;
        std::int32_t count{ 1 };
    };

    struct Cobj {
        std::uint32_t         id{ 0 };
        std::string           edid;
        FormRef               workbench;  // e.g. CraftingSmithingForge
        std::uint32_t         created{ 0 };  // own ARMO object id
        std::vector<CobjItem> items;
        std::uint16_t         count{ 1 };
    };

    struct PluginSpec {
        std::string author;
        std::string description;
        // Preferred master order (the load order). Masters not listed here are
        // appended in first-seen order.
        std::vector<std::string> masterOrder;
        bool                     light{ true };
        float                    hedrVersion{ 1.70f };
        std::vector<Armo>        armos;
        std::vector<Arma>        armas;
        std::vector<Cobj>        cobjs;
    };

    namespace detail {
        inline void PutU8(std::vector<std::uint8_t>& o, std::uint8_t v) { o.push_back(v); }
        inline void PutU16(std::vector<std::uint8_t>& o, std::uint16_t v) {
            o.push_back(static_cast<std::uint8_t>(v & 0xFF));
            o.push_back(static_cast<std::uint8_t>(v >> 8));
        }
        inline void PutU32(std::vector<std::uint8_t>& o, std::uint32_t v) {
            for (int i = 0; i < 4; ++i) {
                o.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
            }
        }
        inline void PutU64(std::vector<std::uint8_t>& o, std::uint64_t v) {
            for (int i = 0; i < 8; ++i) {
                o.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
            }
        }
        inline void PutF32(std::vector<std::uint8_t>& o, float f) {
            std::uint32_t v;
            static_assert(sizeof(v) == sizeof(f));
            std::memcpy(&v, &f, sizeof(v));
            PutU32(o, v);
        }
        inline void PutTag(std::vector<std::uint8_t>& o, const char (&t)[5]) {
            o.insert(o.end(), t, t + 4);
        }
        inline void PatchU32(std::vector<std::uint8_t>& o, std::size_t at, std::uint32_t v) {
            for (int i = 0; i < 4; ++i) {
                o[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
            }
        }

        [[nodiscard]] inline std::string Lower(std::string_view s) {
            std::string r(s);
            for (auto& c : r) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return r;
        }

        // One subrecord: tag, uint16 size, data. Nothing this writer emits
        // comes near 64 KiB, so XXXX extended sizes are not needed.
        class Sub {
        public:
            Sub(std::vector<std::uint8_t>& a_out, const char (&a_tag)[5]) : out_(a_out) {
                PutTag(out_, a_tag);
                sizeAt_ = out_.size();
                PutU16(out_, 0);
                start_ = out_.size();
            }
            ~Sub() {
                const auto n = out_.size() - start_;
                out_[sizeAt_]     = static_cast<std::uint8_t>(n & 0xFF);
                out_[sizeAt_ + 1] = static_cast<std::uint8_t>((n >> 8) & 0xFF);
            }
            Sub(const Sub&)            = delete;
            Sub& operator=(const Sub&) = delete;

        private:
            std::vector<std::uint8_t>& out_;
            std::size_t                sizeAt_{ 0 };
            std::size_t                start_{ 0 };
        };

        inline void ZString(std::vector<std::uint8_t>& o, const char (&a_tag)[5], std::string_view s) {
            Sub sub(o, a_tag);
            o.insert(o.end(), s.begin(), s.end());
            o.push_back(0);
        }

        class Masters {
        public:
            void Note(const FormRef& r) {
                if (r.own || r.file.empty()) {
                    return;
                }
                const auto key = Lower(r.file);
                if (seen_.emplace(key, order_.size()).second) {
                    order_.push_back(r.file);
                }
            }
            // Sort into the preferred (load) order, then freeze the indices.
            void Finish(const std::vector<std::string>& a_preferred) {
                std::map<std::string, std::size_t> rank;
                for (std::size_t i = 0; i < a_preferred.size(); ++i) {
                    rank.emplace(Lower(a_preferred[i]), i);
                }
                std::vector<std::pair<std::size_t, std::string>> keyed;
                for (std::size_t i = 0; i < order_.size(); ++i) {
                    const auto it = rank.find(Lower(order_[i]));
                    // Listed masters first in load order, the rest after, in
                    // the order they were first referenced.
                    const std::size_t k = it != rank.end() ? it->second : a_preferred.size() + i;
                    keyed.emplace_back(k, order_[i]);
                }
                std::stable_sort(keyed.begin(), keyed.end(),
                                 [](const auto& a, const auto& b) { return a.first < b.first; });
                order_.clear();
                index_.clear();
                for (const auto& [k, name] : keyed) {
                    index_.emplace(Lower(name), static_cast<std::uint32_t>(order_.size()));
                    order_.push_back(name);
                }
            }
            [[nodiscard]] const std::vector<std::string>& Names() const { return order_; }
            [[nodiscard]] std::uint32_t                   FormID(const FormRef& r) const {
                if (r.own) {
                    return (static_cast<std::uint32_t>(order_.size()) << 24) | (r.local & 0xFFFFFF);
                }
                if (r.file.empty()) {
                    return 0;
                }
                const auto it = index_.find(Lower(r.file));
                const std::uint32_t idx = it != index_.end() ? it->second : 0;
                return (idx << 24) | (r.local & 0xFFFFFF);
            }

        private:
            std::vector<std::string>             order_;
            std::map<std::string, std::size_t>   seen_;
            std::map<std::string, std::uint32_t> index_;
        };

        // Record wrapper: writes the header, then patches dataSize on close.
        class Record {
        public:
            Record(std::vector<std::uint8_t>& a_out, const char (&a_tag)[5], std::uint32_t a_flags,
                   std::uint32_t a_formID) : out_(a_out) {
                PutTag(out_, a_tag);
                sizeAt_ = out_.size();
                PutU32(out_, 0);       // dataSize, patched
                PutU32(out_, a_flags);
                PutU32(out_, a_formID);
                PutU16(out_, 0);       // timestamp
                PutU16(out_, 0);       // version control
                PutU16(out_, 44);      // form version (SSE)
                PutU16(out_, 0);
                start_ = out_.size();
            }
            ~Record() { PatchU32(out_, sizeAt_, static_cast<std::uint32_t>(out_.size() - start_)); }
            Record(const Record&)            = delete;
            Record& operator=(const Record&) = delete;

        private:
            std::vector<std::uint8_t>& out_;
            std::size_t                sizeAt_{ 0 };
            std::size_t                start_{ 0 };
        };

        class Group {
        public:
            Group(std::vector<std::uint8_t>& a_out, const char (&a_label)[5]) : out_(a_out) {
                start_ = out_.size();
                PutTag(out_, "GRUP");
                PutU32(out_, 0);  // groupSize, patched (header included)
                PutTag(out_, a_label);
                PutU32(out_, 0);  // top-level group
                PutU16(out_, 0);  // timestamp
                PutU16(out_, 0);  // version control
                PutU32(out_, 0);  // unknown
            }
            ~Group() { PatchU32(out_, start_ + 4, static_cast<std::uint32_t>(out_.size() - start_)); }
            Group(const Group&)            = delete;
            Group& operator=(const Group&) = delete;

        private:
            std::vector<std::uint8_t>& out_;
            std::size_t                start_{ 0 };
        };

        inline void FormIdSub(std::vector<std::uint8_t>& o, const char (&a_tag)[5], std::uint32_t a_id) {
            Sub sub(o, a_tag);
            PutU32(o, a_id);
        }
    }

    // Serializes the whole plugin. Records with an empty required reference
    // (an ARMA without a race, say) are still written; validating the spec is
    // the caller's job, which knows what it could and could not resolve.
    [[nodiscard]] inline std::vector<std::uint8_t> Serialize(const PluginSpec& a_spec) {
        using namespace detail;
        Masters masters;
        for (const auto& a : a_spec.armas) {
            masters.Note(a.race);
            for (const auto& r : a.additionalRaces) {
                masters.Note(r);
            }
        }
        for (const auto& a : a_spec.armos) {
            masters.Note(a.race);
            for (const auto& k : a.keywords) {
                masters.Note(k);
            }
        }
        for (const auto& c : a_spec.cobjs) {
            masters.Note(c.workbench);
            for (const auto& i : c.items) {
                masters.Note(i.item);
            }
        }
        masters.Finish(a_spec.masterOrder);

        std::uint32_t maxId = 0x7FF;
        for (const auto& a : a_spec.armas) maxId = std::max(maxId, a.id);
        for (const auto& a : a_spec.armos) maxId = std::max(maxId, a.id);
        for (const auto& c : a_spec.cobjs) maxId = std::max(maxId, c.id);

        std::uint32_t groups = 0;
        groups += a_spec.armos.empty() ? 0 : 1;
        groups += a_spec.armas.empty() ? 0 : 1;
        groups += a_spec.cobjs.empty() ? 0 : 1;
        const auto records = static_cast<std::uint32_t>(a_spec.armos.size() + a_spec.armas.size() +
                                                        a_spec.cobjs.size());

        std::vector<std::uint8_t> o;
        {
            Record tes4(o, "TES4", a_spec.light ? 0x200u : 0u, 0);
            {
                Sub hedr(o, "HEDR");
                PutF32(o, a_spec.hedrVersion);
                PutU32(o, records + groups);
                PutU32(o, maxId + 1);
            }
            ZString(o, "CNAM", a_spec.author.empty() ? std::string_view("Fitting Room")
                                                     : std::string_view(a_spec.author));
            if (!a_spec.description.empty()) {
                ZString(o, "SNAM", a_spec.description);
            }
            for (const auto& m : masters.Names()) {
                ZString(o, "MAST", m);
                Sub data(o, "DATA");
                PutU64(o, 0);
            }
        }

        // Canonical top-group order: ARMO, COBJ, ARMA.
        if (!a_spec.armos.empty()) {
            Group g(o, "ARMO");
            for (const auto& a : a_spec.armos) {
                Record r(o, "ARMO", 0, masters.FormID(FormRef::Own(a.id)));
                ZString(o, "EDID", a.edid);
                {
                    Sub obnd(o, "OBND");
                    for (int i = 0; i < 6; ++i) {
                        PutU16(o, 0);
                    }
                }
                ZString(o, "FULL", a.name);
                if (!a.worldModel[0].empty()) {
                    ZString(o, "MOD2", a.worldModel[0]);
                }
                if (!a.worldModel[1].empty()) {
                    ZString(o, "MOD4", a.worldModel[1]);
                }
                {
                    Sub bod2(o, "BOD2");
                    PutU32(o, a.slots);
                    PutU32(o, a.armorType);
                }
                if (!a.race.Empty()) {
                    FormIdSub(o, "RNAM", masters.FormID(a.race));
                }
                if (!a.keywords.empty()) {
                    {
                        Sub ksiz(o, "KSIZ");
                        PutU32(o, static_cast<std::uint32_t>(a.keywords.size()));
                    }
                    Sub kwda(o, "KWDA");
                    for (const auto& k : a.keywords) {
                        PutU32(o, masters.FormID(k));
                    }
                }
                ZString(o, "DESC", "");
                for (const auto id : a.addons) {
                    FormIdSub(o, "MODL", masters.FormID(FormRef::Own(id)));
                }
                {
                    Sub data(o, "DATA");
                    PutU32(o, static_cast<std::uint32_t>(a.value));
                    PutF32(o, a.weight);
                }
                {
                    Sub dnam(o, "DNAM");
                    const float scaled = a.rating * 100.0f;
                    PutU32(o, static_cast<std::uint32_t>(
                                  static_cast<std::int32_t>(scaled + (scaled >= 0.0f ? 0.5f : -0.5f))));
                }
            }
        }

        if (!a_spec.cobjs.empty()) {
            Group g(o, "COBJ");
            for (const auto& c : a_spec.cobjs) {
                Record r(o, "COBJ", 0, masters.FormID(FormRef::Own(c.id)));
                ZString(o, "EDID", c.edid);
                if (!c.items.empty()) {
                    {
                        Sub coct(o, "COCT");
                        PutU32(o, static_cast<std::uint32_t>(c.items.size()));
                    }
                    for (const auto& it : c.items) {
                        Sub cnto(o, "CNTO");
                        PutU32(o, masters.FormID(it.item));
                        PutU32(o, static_cast<std::uint32_t>(it.count));
                    }
                }
                FormIdSub(o, "CNAM", masters.FormID(FormRef::Own(c.created)));
                if (!c.workbench.Empty()) {
                    FormIdSub(o, "BNAM", masters.FormID(c.workbench));
                }
                {
                    Sub nam1(o, "NAM1");
                    PutU16(o, c.count);
                }
            }
        }

        if (!a_spec.armas.empty()) {
            Group g(o, "ARMA");
            for (const auto& a : a_spec.armas) {
                Record r(o, "ARMA", 0, masters.FormID(FormRef::Own(a.id)));
                ZString(o, "EDID", a.edid);
                {
                    Sub bod2(o, "BOD2");
                    PutU32(o, a.slots);
                    PutU32(o, a.armorType);
                }
                if (!a.race.Empty()) {
                    FormIdSub(o, "RNAM", masters.FormID(a.race));
                }
                {
                    // 12 bytes: priority m/f, weight slider m/f, 2 unknown,
                    // detection sound value, 1 unknown, weapon adjust.
                    Sub dnam(o, "DNAM");
                    PutU8(o, static_cast<std::uint8_t>(a.priority[0]));
                    PutU8(o, static_cast<std::uint8_t>(a.priority[1]));
                    PutU8(o, static_cast<std::uint8_t>(a.weightSlider[0]));
                    PutU8(o, static_cast<std::uint8_t>(a.weightSlider[1]));
                    PutU8(o, 0);
                    PutU8(o, 0);
                    PutU8(o, static_cast<std::uint8_t>(a.detectionSoundValue));
                    PutU8(o, 0);
                    PutF32(o, a.weaponAdjust);
                }
                if (!a.model[0].empty()) {
                    ZString(o, "MOD2", a.model[0]);
                }
                if (!a.model[1].empty()) {
                    ZString(o, "MOD3", a.model[1]);
                }
                if (!a.model1st[0].empty()) {
                    ZString(o, "MOD4", a.model1st[0]);
                }
                if (!a.model1st[1].empty()) {
                    ZString(o, "MOD5", a.model1st[1]);
                }
                for (const auto& race : a.additionalRaces) {
                    FormIdSub(o, "MODL", masters.FormID(race));
                }
            }
        }
        return o;
    }

}  // namespace OS::EspWriter
