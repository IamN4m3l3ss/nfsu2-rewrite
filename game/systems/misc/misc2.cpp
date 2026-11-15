// zMisc batch 2, written from the PC machine code: replay-manager event lists, bit writer, running average,
// the null file driver's virtuals.
#include <cstring>
#include <string>
#include <vector>

#include "diff.h"
#include "regs.h"
#include "rewrite.h"
#include "x87.h"

namespace {

template <class T> T& at(void* p, uint32_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + off); }
inline int32_t str_cmp(const char* a, const char* b) { return game_fn<int32_t(__cdecl*)(const char*, const char*)>(0x0043DC30)(a, b); }

// ---------------------------------------------------------------- ReplayManager: enabled +4, snapshot list sentinel
// +0x98 {next, prev}; snapshot: name +8, interest +0x1D8
// 0x00578E90 RegisterInterestingEvent(float delta, ...): every snapshot named like 0x79545C gets interest += delta
// (the second argument is not used)
using Reg_t = void(__thiscall*)(void*, float, uint32_t);
Reg_t o_RegisterInterestingEvent = nullptr;
void __cdecl RegisterInterestingEvent(void* m, float delta, uint32_t) {
    if (!at<uint32_t>(m, 4)) return;
    uint8_t* const end = static_cast<uint8_t*>(m) + 0x98;
    for (uint8_t* n = at<uint8_t*>(m, 0x98); n != end; n = *reinterpret_cast<uint8_t**>(n))
        if (str_cmp(reinterpret_cast<char*>(n + 8), reinterpret_cast<const char*>(0x0079545C)) == 0)
            at<float>(n, 0x1D8) = delta + at<float>(n, 0x1D8);
}
// 0x00578EE0 InvalidateInterestingEvents = RegisterInterestingEvent(-99, 0x7A0820)
using Inv_t = void(__thiscall*)(void*);
Inv_t o_InvalidateInterestingEvents = nullptr;
void __cdecl InvalidateInterestingEvents(void* m) { RegisterInterestingEvent(m, -99.0f, 0x007A0820); }
// 0x00578FD0 FindSnapshot(name, n): the n-th snapshot with that name, or null
using Find_t = void*(__thiscall*)(void*, const char*, int32_t);
Find_t o_FindSnapshot = nullptr;
void* __cdecl FindSnapshot(void* m, const char* name, int32_t n) {
    uint8_t* const end = static_cast<uint8_t*>(m) + 0x98;
    for (uint8_t* s = at<uint8_t*>(m, 0x98); s != end; s = *reinterpret_cast<uint8_t**>(s)) {
        if (str_cmp(reinterpret_cast<char*>(s + 8), name) != 0) continue;
        if (n == 0) return s;
        --n;
    }
    return nullptr;
}

// ---------------------------------------------------------------- 0x00574F70 bRepeat::WriteToLocation(bit, value, nbits)
// bits of `value` (LSB first, `sar` by the bit index) into the bit stream at `this`
using Write_t = void(__thiscall*)(void*, int32_t, int32_t, int32_t);
Write_t o_WriteToLocation = nullptr;
void __cdecl WriteToLocation(void* self, int32_t bit, int32_t value, int32_t nbits) {
    uint8_t* p = static_cast<uint8_t*>(self) + ((bit + ((bit >> 31) & 7)) >> 3);
    int32_t b = bit & 7;
    for (int32_t i = 0; i < nbits; ++i) {
        const uint8_t d = static_cast<uint8_t>(((value >> (i & 31)) & 1) << b);
        const uint8_t keep = static_cast<uint8_t>(~(1u << b));
        *p = static_cast<uint8_t>(d | (keep & *p));
        b = (b + 1) & 7;
        if (b == 0) ++p;
    }
}

// ---------------------------------------------------------------- Average: size +5, count +6, index +7 (u8),
// sum +8, average +0xC, samples +0x10 (float*)
// 0x00576970 Record(v)
using Rec_t = void(__thiscall*)(void*, float);
Rec_t o_AverageRecord = nullptr;
void __cdecl AverageRecord(void* a, float v) {
    if (at<uint8_t>(a, 6) < at<uint8_t>(a, 5)) ++at<uint8_t>(a, 6);
    float* slot = at<float*>(a, 0x10) + at<uint8_t>(a, 7);
    const float s = v + at<float>(a, 8);
    at<float>(a, 8) = s;
    at<float>(a, 8) = s - *slot;
    std::memcpy(slot, &v, 4);
    at<float>(a, 0xC) = at<float>(a, 8) / static_cast<float>(static_cast<int32_t>(at<uint8_t>(a, 6)));
    if (++at<uint8_t>(a, 7) >= at<uint8_t>(a, 5)) at<uint8_t>(a, 7) = 0;
}
// 0x00576A10 Flush(v): every sample = v (fld/fstp: a signalling NaN comes out quiet), count = size,
// average = v (raw copy), sum = size * v
Rec_t o_AverageFlush = nullptr;
void __cdecl AverageFlush(void* a, float v) {
    for (int32_t i = 0; i < at<uint8_t>(a, 5); ++i) at<float*>(a, 0x10)[i] = x87::st(x87::ld(v));
    at<uint8_t>(a, 6) = at<uint8_t>(a, 5);
    std::memcpy(&at<float>(a, 0xC), &v, 4);
    at<float>(a, 8) = x87::st(x87::mul(x87::ild(at<uint8_t>(a, 5)), x87::ld(v)));
}

// ---------------------------------------------------------------- NullFileDriver virtuals
using Open_t = int32_t(__thiscall*)(void*, uint32_t, uint32_t, uint32_t);
Open_t o_NullOpen = nullptr;
int32_t __cdecl NullOpen(void*, uint32_t, uint32_t, uint32_t) { return -1; }  // 0x006F7EA7
using Read_t = int32_t(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
Read_t o_NullRead = nullptr;
int32_t __cdecl NullRead(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }  // 0x006F7EAD

}  // namespace

NFSU2_REWRITE_THIS(0x00578E90, RegisterInterestingEvent, 2, "void ReplayManager::RegisterInterestingEvent(float delta, ...)", o_RegisterInterestingEvent);
NFSU2_REWRITE_THIS(0x00578EE0, InvalidateInterestingEvents, 0, "void ReplayManager::InvalidateInterestingEvents()", o_InvalidateInterestingEvents);
NFSU2_REWRITE_THIS(0x00578FD0, FindSnapshot, 2, "ReplaySnapshot* ReplayManager::FindSnapshot(const char* name, int n)", o_FindSnapshot);
NFSU2_REWRITE_THIS(0x00574F70, WriteToLocation, 3, "void bRepeat::WriteToLocation(int bit, int value, int nbits)", o_WriteToLocation);
NFSU2_REWRITE_THIS(0x00576970, AverageRecord, 1, "void Average::Record(float v)", o_AverageRecord);
NFSU2_REWRITE_THIS(0x00576A10, AverageFlush, 1, "void Average::Flush(float v)", o_AverageFlush);
NFSU2_REWRITE_THIS(0x006F7EA7, NullOpen, 3, "int NullFileDriver::Open(...) -> -1", o_NullOpen);
NFSU2_REWRITE_THIS(0x006F7EAD, NullRead, 5, "int NullFileDriver::Read(...) -> 0", o_NullRead);

namespace {

float rf(game::Rng& r) {
    switch (r.below(8)) {
        case 0: return 0.0f;
        case 1: return static_cast<float>(static_cast<int32_t>(r.below(9)) - 4);
        case 2: { const uint32_t u = 0x7FA00001u; float f; std::memcpy(&f, &u, 4); return f; }  // signalling NaN
        default: return (static_cast<float>(r.next() >> 8) / 16777216.0f - 0.5f) * (r.below(2) ? 8.0f : 2000.0f);
    }
}
bool same(float a, float b) { return game::same_float(a, b); }

// a fake ReplayManager with n snapshots, names drawn from a small set (incl. the "interesting" name)
struct FakeManager {
    std::vector<uint8_t> mgr = std::vector<uint8_t>(0x100);
    std::vector<std::vector<uint8_t>> nodes;
    FakeManager(game::Rng& r) {
        const char* interesting = reinterpret_cast<const char*>(0x0079545C);
        const char* names[] = {interesting, "SNAP_A", "SNAP_B", ""};
        at<uint32_t>(mgr.data(), 4) = r.below(4) ? 1 : 0;
        uint8_t* sent = mgr.data() + 0x98;
        uint8_t* prev = sent;
        const uint32_t n = r.below(8);
        nodes.resize(n);
        for (uint32_t i = 0; i < n; ++i) {
            nodes[i].assign(0x1E0, 0);
            std::strncpy(reinterpret_cast<char*>(nodes[i].data() + 8), names[r.below(4)], 64);
            at<float>(nodes[i].data(), 0x1D8) = rf(r);
            *reinterpret_cast<uint8_t**>(prev) = nodes[i].data();
            prev = nodes[i].data();
        }
        *reinterpret_cast<uint8_t**>(prev) = sent;
    }
    std::vector<uint32_t> interest() const {
        std::vector<uint32_t> v;
        for (auto& n : nodes) v.push_back(*reinterpret_cast<const uint32_t*>(n.data() + 0x1D8));
        return v;
    }
};

}  // namespace

NFSU2_DIFF_TEST(zMisc2) {
    return game::run_diff(ctx, "zMisc2", o_AverageRecord, [](game::Rng& r) -> game::Case {
        const uint32_t which = r.below(7);
        if (which <= 1) {
            game::Rng r2 = r;
            FakeManager a(r);
            FakeManager b(r2);  // same draws -> same structure
            const float d = rf(r);
            if (which == 0) o_RegisterInterestingEvent(a.mgr.data(), d, 0), RegisterInterestingEvent(b.mgr.data(), d, 0);
            else o_InvalidateInterestingEvents(a.mgr.data()), InvalidateInterestingEvents(b.mgr.data());
            const auto ia = a.interest(), ib = b.interest();
            for (size_t i = 0; i < ia.size(); ++i) {
                float x, y;
                std::memcpy(&x, &ia[i], 4);
                std::memcpy(&y, &ib[i], 4);
                if (!same(x, y)) return game::MismatchText("RegisterInterestingEvent");
            }
            return ia.size() == ib.size() ? game::Ok() : game::MismatchText("shape");
        }
        if (which == 2) {
            FakeManager m(r);
            const char* names[] = {"SNAP_A", "SNAP_B", "", "none"};
            const char* nm = names[r.below(4)];
            const int32_t k = static_cast<int32_t>(r.below(4)) - (r.below(8) == 0 ? 2 : 0);
            return o_FindSnapshot(m.mgr.data(), nm, k) == FindSnapshot(m.mgr.data(), nm, k) ? game::Ok() : game::MismatchText("FindSnapshot");
        }
        if (which == 3) {
            uint8_t a[64], b[64];
            for (auto& x : a) x = static_cast<uint8_t>(r.next());
            std::memcpy(b, a, sizeof a);
            const int32_t bit = static_cast<int32_t>(r.below(256)), val = static_cast<int32_t>(r.next()), n = static_cast<int32_t>(r.below(40));
            o_WriteToLocation(a, bit, val, n);
            WriteToLocation(b, bit, val, n);
            return std::memcmp(a, b, sizeof a) == 0 ? game::Ok() : game::MismatchText("WriteToLocation");
        }
        if (which <= 5) {  // Average: a few Records / a Flush on two identical objects
            float sa[16], sb[16];
            for (float& f : sa) f = rf(r);
            std::memcpy(sb, sa, sizeof sa);
            uint8_t a[0x14] = {}, b[0x14] = {};
            at<uint8_t>(a, 5) = static_cast<uint8_t>(1 + r.below(16));
            at<uint8_t>(a, 6) = static_cast<uint8_t>(r.below(at<uint8_t>(a, 5) + 1));
            at<uint8_t>(a, 7) = static_cast<uint8_t>(r.below(at<uint8_t>(a, 5)));
            at<float>(a, 8) = rf(r);
            std::memcpy(b, a, sizeof a);
            at<float*>(a, 0x10) = sa;
            at<float*>(b, 0x10) = sb;
            for (int i = 0, n = 1 + r.below(6); i < n; ++i) {
                const float v = rf(r);
                if (which == 5 && i == 0) o_AverageFlush(a, v), AverageFlush(b, v);
                else o_AverageRecord(a, v), AverageRecord(b, v);
            }
            for (int i = 0; i < 16; ++i) if (!same(sa[i], sb[i])) return game::MismatchText("samples");
            return std::memcmp(a, b, 8) == 0 && same(at<float>(a, 8), at<float>(b, 8)) && same(at<float>(a, 0xC), at<float>(b, 0xC))
                       ? game::Ok() : game::MismatchText("Average");
        }
        uint8_t self[8];
        return o_NullOpen(self, r.next(), r.next(), r.next()) == NullOpen(self, 0, 0, 0) &&
                       o_NullRead(self, r.next(), 0, 0, 0, 0) == NullRead(self, 0, 0, 0, 0, 0)
                   ? game::Ok() : game::MismatchText("NullFileDriver");
    });
}

NFSU2_DIFF_TEST(regs_zMisc2) {  // FindSnapshot / NullFileDriver EAX + pops; the void ones: pops
    return game::run_diff(ctx, "regs_zMisc2", o_FindSnapshot, [](game::Rng& r) -> game::Case {
        FakeManager m(r);
        uint8_t self[8], bits[64] = {};
        const uint32_t nm = static_cast<uint32_t>(reinterpret_cast<uintptr_t>("SNAP_A"));
        for (auto c : {game::check_regs_this("FindSnapshot", (void*)o_FindSnapshot, NFSU2_ENTRY(FindSnapshot), m.mgr.data(), m.mgr.data(), {nm, r.below(3)}),
                       game::check_regs_this("NullOpen", (void*)o_NullOpen, NFSU2_ENTRY(NullOpen), self, self, {1, 2, 3}),
                       game::check_regs_this("NullRead", (void*)o_NullRead, NFSU2_ENTRY(NullRead), self, self, {1, 2, 3, 4, 5}),
                       game::check_regs_this("WriteToLocation", (void*)o_WriteToLocation, NFSU2_ENTRY(WriteToLocation), bits, bits, {r.below(64), r.next(), r.below(16)}, false)})
            if (!c.ok) return c;
        return game::Ok();
    });
}

NFSU2_DIFF_TEST(zMisc2_BROKEN) {  // harness self-check: bit writer MSB-first
    return game::run_diff(ctx, "zMisc2_BROKEN", o_WriteToLocation, [](game::Rng& r) -> game::Case {
        uint8_t a[16] = {}, b[16] = {};
        const int32_t val = static_cast<int32_t>(r.next() | 1), n = 2 + static_cast<int32_t>(r.below(20));
        o_WriteToLocation(a, 0, val, n);
        for (int32_t i = 0; i < n; ++i) if ((val >> (n - 1 - i)) & 1) b[i / 8] |= static_cast<uint8_t>(1 << (i % 8));
        return std::memcmp(a, b, sizeof a) == 0 ? game::Ok() : game::MismatchText("MSB first");
    });
}
