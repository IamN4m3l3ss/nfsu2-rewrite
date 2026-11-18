// zMisc leaf batch 1 (38), written from the PC machine code: tCubic1D/2D/3D interpolators, Linear / Average /
// PidError helpers, Timer::GetHoursMinsSeconds, bRepeat bit reader, ReplayManager snapshot list, DemoModeReplay
// chunk loader, GameFlowManager track load/unload state, joystick / region / loading queries, DeviceDriver ctor.
// Table-driven leaf tests (game/framework/leaf.h); x87 maths through x87.h.
#include "leaf.h"
#include "live.h"
#include "x87.h"

namespace {
using namespace x87;

// ---- tCubic1D / 2D / 3D --------------------------------------------------------------------------------------

// 0x00574830 tCubic1D::MakeCoeffs: from value a/b (+0, +4) and slopes c/d (+8, +0xC): +0x10 = (b + d) - 2(c - a),
// +0x14 = (c - a) * K - d - 2b, +0x18 = b, +0x1C = a (raw). EAX = b's bits.
uint32_t __cdecl Cubic1D_MakeCoeffs(uint8_t* s) {
    const uint32_t a = at<uint32_t>(s, 0), b = at<uint32_t>(s, 4);
    const X t = sub(ld(at<float>(s, 8)), ld(at<float>(s, 0)));
    const X u = add(ld(at<float>(s, 4)), ld(at<float>(s, 0xC)));
    at<uint32_t>(s, 0x18) = b;
    at<uint32_t>(s, 0x1C) = a;
    at<float>(s, 0x10) = st(sub(u, add(t, t)));
    const X v = sub(mul(t, ld(gf(0x00787ADCu))), ld(at<float>(s, 0xC)));
    const X b2 = ld(at<float>(s, 4));
    at<float>(s, 0x14) = st(sub(v, add(b2, b2)));
    return b;
}

// 0x00574890 tCubic1D::GetddVal(t) = t * +0x10 * K + 2 * +0x14
float __cdecl Cubic1D_GetddVal(uint8_t* s, uint32_t tb) {
    float t;
    std::memcpy(&t, &tb, 4);
    const X c = ld(at<float>(s, 0x14));
    return st(add(mul(mul(ld(t), ld(at<float>(s, 0x10))), ld(gf(0x007859D0u))), add(c, c)));
}

// tCubic2D setters: store v.x / v.y into (xo, yo); a component that differs from (xc, yc) (or is NaN) marks the
// axis dirty (word +0x2C / +0x5C = 2). EAX = high half of the vector pointer + the last x87 status word.
uint32_t cubic2d_set(uint8_t* s, uint32_t vp, uint32_t xo, uint32_t xc, uint32_t yo, uint32_t yc) {
    const float* v = reinterpret_cast<const float*>(vp);
    const X y = ld(v[1]), x = ld(v[0]);
    at<float>(s, xo) = st(x);
    if (parity_even(static_cast<uint8_t>(cmpw(x, ld(at<float>(s, xc))) >> 8) & 0x44)) at<uint16_t>(s, 0x2C) = 2;
    at<float>(s, yo) = st(y);
    const uint16_t sw = cmpw(y, ld(at<float>(s, yc)));
    if (parity_even(static_cast<uint8_t>(sw >> 8) & 0x44)) at<uint16_t>(s, 0x5C) = 2;
    return (vp & 0xFFFF0000u) | sw;
}
// 0x00574B10 tCubic2D::SetVal
uint32_t __cdecl Cubic2D_SetVal(uint8_t* s, uint32_t vp) { return cubic2d_set(s, vp, 0x00, 0x08, 0x30, 0x38); }
// 0x00574B50 tCubic2D::SetdVal
uint32_t __cdecl Cubic2D_SetdVal(uint8_t* s, uint32_t vp) { return cubic2d_set(s, vp, 0x04, 0x0C, 0x34, 0x3C); }
// 0x00574B90 tCubic2D::SetValDesired
uint32_t __cdecl Cubic2D_SetValDesired(uint8_t* s, uint32_t vp) { return cubic2d_set(s, vp, 0x08, 0x00, 0x38, 0x30); }

// 0x00574BD0 tCubic2D::SetdValDesired: +0xC = v.x (raw), +0x3C = v.y. EAX = v.x's bits.
uint32_t __cdecl Cubic2D_SetdValDesired(uint8_t* s, uint32_t vp) {
    const float* v = reinterpret_cast<const float*>(vp);
    const X y = ld(v[1]);
    const uint32_t x = *reinterpret_cast<const uint32_t*>(v);
    at<float>(s, 0x3C) = st(y);
    at<uint32_t>(s, 0x0C) = x;
    return x;
}
// 0x00574CF0 tCubic3D::SetdValDesired: +0xC = v.x (raw), +0x3C = v.y, +0x6C = v.z. EAX = v.x's bits.
uint32_t __cdecl Cubic3D_SetdValDesired(uint8_t* s, uint32_t vp) {
    const float* v = reinterpret_cast<const float*>(vp);
    const X z = ld(v[2]), y = ld(v[1]);
    const uint32_t x = *reinterpret_cast<const uint32_t*>(v);
    at<float>(s, 0x3C) = st(y);
    at<uint32_t>(s, 0x0C) = x;
    at<float>(s, 0x6C) = st(z);
    return x;
}

// raw two/three-field getters into out: EAX = out
uint32_t copy_out(uint8_t* s, uint32_t op, std::initializer_list<uint32_t> offs) {
    uint32_t* out = reinterpret_cast<uint32_t*>(op);
    for (uint32_t o : offs) *out++ = at<uint32_t>(s, o);
    return op;
}
// 0x00574BF0 tCubic2D::GetVal(out)
uint32_t __cdecl Cubic2D_GetVal(uint8_t* s, uint32_t op) { return copy_out(s, op, {0x00, 0x30}); }
// 0x00574C10 tCubic2D::GetdVal(out)
uint32_t __cdecl Cubic2D_GetdVal(uint8_t* s, uint32_t op) { return copy_out(s, op, {0x04, 0x34}); }
// 0x00574C70 tCubic2D::GetValDesired(out)
uint32_t __cdecl Cubic2D_GetValDesired(uint8_t* s, uint32_t op) { return copy_out(s, op, {0x08, 0x38}); }
// 0x00574DD0 tCubic3D::GetValDesired(out)
uint32_t __cdecl Cubic3D_GetValDesired(uint8_t* s, uint32_t op) { return copy_out(s, op, {0x08, 0x38, 0x68}); }

// one cubic row at t: ((t * c0 + c1) * t + c2) * t + c3, coefficients at +k .. +k+0xC
float cubic_row(uint8_t* s, uint32_t k, float t) {
    const X tx = ld(t);
    return st(add(mul(add(mul(add(mul(tx, ld(at<float>(s, k))), ld(at<float>(s, k + 4))), tx), ld(at<float>(s, k + 8))), tx),
                  ld(at<float>(s, k + 0xC))));
}
// 0x00574C30 tCubic2D::GetVal(out, t): rows +0x10 and +0x40. EAX = out.
uint32_t __cdecl Cubic2D_GetValAt(uint8_t* s, uint32_t op, uint32_t tb) {
    float t;
    std::memcpy(&t, &tb, 4);
    float* out = reinterpret_cast<float*>(op);
    out[0] = cubic_row(s, 0x10, t);
    out[1] = cubic_row(s, 0x40, t);
    return op;
}
// 0x00574D70 tCubic3D::GetVal(out, t): rows +0x10, +0x40, +0x70. EAX = out.
uint32_t __cdecl Cubic3D_GetValAt(uint8_t* s, uint32_t op, uint32_t tb) {
    float t;
    std::memcpy(&t, &tb, 4);
    float* out = reinterpret_cast<float*>(op);
    out[0] = cubic_row(s, 0x10, t);
    out[1] = cubic_row(s, 0x40, t);
    out[2] = cubic_row(s, 0x70, t);
    return op;
}

// ---- Linear / Average / PidError / Graph ---------------------------------------------------------------------

// 0x00576C60 Linear::Init(x0, y0, x1, y1): slope +0 = (y1 - y0) / (x1 - x0), offset +4 = y0 - slope * x0.
// EAX is not touched.
uint32_t __cdecl Linear_Init(uint8_t* s, uint32_t x0b, uint32_t y0b, uint32_t x1b, uint32_t y1b, uint32_t eax_in) {
    float x0, y0, x1, y1;
    std::memcpy(&x0, &x0b, 4), std::memcpy(&y0, &y0b, 4), std::memcpy(&x1, &x1b, 4), std::memcpy(&y1, &y1b, 4);
    const X m = div(sub(ld(y1), ld(y0)), sub(ld(x1), ld(x0)));
    at<float>(s, 0) = st(m);
    at<float>(s, 4) = st(sub(ld(y0), mul(m, ld(x0))));
    return eax_in;
}
// 0x00576C90 Linear::GetValue(x) = x * slope + offset
float __cdecl Linear_GetValue(uint8_t* s, uint32_t xb) {
    float x;
    std::memcpy(&x, &xb, 4);
    return st(add(mul(ld(x), ld(at<float>(s, 0))), ld(at<float>(s, 4))));
}
// 0x00576910 Average::Recalculate: sum +8 = sum * 0 + samples[[+0x10]][0..count +5), average +0xC = sum / max(1,
// filled +6). EAX = filled.
uint32_t __cdecl Average_Recalculate(uint8_t* s) {
    at<float>(s, 8) = st(mul(ld(at<float>(s, 8)), ld(gf(0x007D7D3Cu))));
    const float* p = ptr(s, 0x10);
    for (uint32_t i = 0; i < s[5]; ++i) at<float>(s, 8) = st(add(ld(p[i]), ld(at<float>(s, 8))));
    const uint32_t filled = s[6];
    const int32_t k = filled >= 1 ? static_cast<int32_t>(filled) : 1;
    at<float>(s, 0xC) = st(mul(div(ld(gf(0x00784250u)), ild(k)), ld(at<float>(s, 8))));
    return filled;
}
// 0x005769D0 PidError::ResetIntegral(v): every sample = v, filled +6 = 0, sum +8 = count * v. EAX = count.
uint32_t __cdecl PidError_ResetIntegral(uint8_t* s, uint32_t vb) {
    float v;
    std::memcpy(&v, &vb, 4);
    for (uint32_t i = 0; i < s[5]; ++i) at<float>(ptr(s, 0x10), i * 4) = st(ld(v));
    const uint32_t n = s[5];
    s[6] = 0;
    at<float>(s, 8) = st(mul(ild(static_cast<int32_t>(n)), ld(v)));
    return n;
}
// 0x00576770 Graph::Graph(points, count). EAX = this.
uint32_t __cdecl Graph_ctor(uint8_t* s, uint32_t a, uint32_t b) {
    at<uint32_t>(s, 0) = a;
    at<uint32_t>(s, 4) = b;
    return u(s);
}

// ---- time / bits / files ---------------------------------------------------------------------------------------

// 0x00576CE0 GetRealTimeElapsedFromQuantized(q) = q * 0x7A0618
float __cdecl RealTimeFromQuantized(uint32_t q) { return st(mul(ild(static_cast<int32_t>(q)), ld(gf(0x007A0618u)))); }

// 0x005771B0 Timer::GetHoursMinsSeconds(h*, m*, s*, ms*): ticks +0 at 4000/s. EAX = sign of the ms quotient.
uint32_t __cdecl Timer_GetHMS(uint8_t* s, uint32_t hp, uint32_t mp, uint32_t sp, uint32_t msp) {
    const int32_t t = at<int32_t>(s, 0);
    const int32_t sec = t / 4000;
    const int32_t rem = static_cast<int32_t>(static_cast<uint32_t>(t - sec * 4000) * 1000u);
    const int32_t h = sec / 3600;
    *reinterpret_cast<int32_t*>(hp) = h;
    const int32_t inh = sec - h * 3600;
    const int32_t m = inh / 60;
    *reinterpret_cast<int32_t*>(mp) = m;
    *reinterpret_cast<int32_t*>(sp) = inh - m * 60;
    *reinterpret_cast<int32_t*>(msp) = rem / 4000;
    return rem < 0 ? 1u : 0u;
}

// 0x00574FD0 bRepeat::ReadFromLocation(bitpos, nbits): nbits bits from this's bit stream, LSB first
uint32_t __cdecl Repeat_ReadFromLocation(uint8_t* s, uint32_t bpos, uint32_t n) {
    const int32_t bp = static_cast<int32_t>(bpos);
    const uint8_t* p = s + bp / 8;
    uint32_t bit = bpos & 7, r = 0;
    for (int32_t i = 0; i < static_cast<int32_t>(n); ++i) {
        r |= ((static_cast<uint32_t>(*p) >> bit) & 1u) << (i & 31);
        bit = (bit + 1) & 7;
        if (bit == 0) ++p;
    }
    return r;
}

// 0x005755E0 bFileSize(file): its size (+4), 0 for none or negative
uint32_t __cdecl FileSize(uint32_t f) {
    if (!f) return 0;
    const int32_t v = at<int32_t>(f, 4);
    return v >= 0 ? static_cast<uint32_t>(v) : 0u;
}

// 0x006F6C23 RealFile::DeviceDriver::DeviceDriver(name): vtable, name copied to +4. EAX = this.
uint32_t __cdecl DeviceDriver_ctor(uint8_t* s, uint32_t name) {
    at<uint32_t>(s, 0) = 0x007A0358;
    const char* n = reinterpret_cast<const char*>(name);
    char* d = reinterpret_cast<char*>(s + 4);
    char c;
    do {
        c = *n++;
        *d++ = c;
    } while (c);
    return u(s);
}

// ---- replay / game flow ------------------------------------------------------------------------------------------

// 0x00571830 ReplayManager::MarkTheEndOfTime: +0x80 = 1 if unset. EAX = the old value.
uint32_t __cdecl Replay_MarkTheEndOfTime(uint8_t* s) {
    const uint32_t t = at<uint32_t>(s, 0x80);
    if (!t) at<uint32_t>(s, 0x80) = 1;
    return t;
}
// 0x00571850 ReplayManager::WasSnapshotTakenAtThisTime(time): search the snapshot times +0xE0[] from the cached
// index +0xD8 (if its time is earlier) up to the count +0xD4; a hit becomes the new cache. AL = found, upper EAX =
// the index.
uint32_t __cdecl Replay_WasSnapshotTaken(uint8_t* s, uint32_t time) {
    const uint32_t d = at<uint32_t>(s, 0xD8);
    uint32_t i = 0;
    if (d != 0 && at<int32_t>(s, 0xE0 + d * 4) < static_cast<int32_t>(time)) i = d;
    if (static_cast<int32_t>(i) >= at<int32_t>(s, 0xD4)) return i & 0xFFFFFF00u;
    for (;;) {
        if (at<uint32_t>(s, 0xE0 + i * 4) == time) {
            at<uint32_t>(s, 0xD8) = i;
            return (i & 0xFFFFFF00u) | 1u;
        }
        ++i;
        if (static_cast<int32_t>(i) >= at<int32_t>(s, 0xD4)) return i & 0xFFFFFF00u;
    }
}
// 0x00579020 ReplayManager::GetSnapshot(n): the n-th node of the list headed at +0x98, 0 past the end
uint32_t __cdecl Replay_GetSnapshot(uint8_t* s, uint32_t n) {
    uint8_t* head = s + 0x98;
    uint8_t* e = ptr(head, 0);
    if (e == head) return 0;
    for (;;) {
        if (n == 0) return u(e);
        e = ptr(e, 0);
        --n;
        if (e == head) return 0;
    }
}
// 0x00579050 ReplayManager::DeleteSnapshot(node): unlink it, append it to the free list at +0x90. EAX = node.
uint32_t __cdecl Replay_DeleteSnapshot(uint8_t* s, uint32_t np) {
    uint8_t* e = reinterpret_cast<uint8_t*>(np);
    uint8_t* next = ptr(e, 0);
    uint8_t* prev = ptr(e, 4);
    at<uint8_t*>(prev, 0) = next;
    at<uint8_t*>(next, 4) = prev;
    uint8_t* tail = ptr(s, 0x94);
    at<uint8_t*>(tail, 0) = e;
    at<uint8_t*>(s, 0x94) = e;
    at<uint8_t*>(e, 4) = tail;
    at<uint8_t*>(e, 0) = s + 0x90;
    return np;
}
// 0x00579260 DemoModeReplay::Loader(chunk): 0x39100 -> header +0x24, 0x39101 -> append to the list at +0x2C,
// 0x39102 -> +0x28 (all point at the chunk data, chunk + 8). EAX = handled.
uint32_t __cdecl DemoReplay_Loader(uint8_t* s, uint32_t cp) {
    const uint32_t id = at<uint32_t>(cp, 0);
    uint8_t* node = reinterpret_cast<uint8_t*>(cp) + 8;
    if (id == 0x39100) {
        at<uint8_t*>(s, 0x24) = node;
        return 1;
    }
    if (id == 0x39101) {
        uint8_t* tail = ptr(s, 0x30);
        at<uint8_t*>(tail, 0) = node;
        at<uint8_t*>(s, 0x30) = node;
        at<uint8_t*>(node, 4) = tail;
        at<uint8_t*>(node, 0) = s + 0x2C;
        at<uint32_t>(node, 0x3C) = 1;
        return 1;
    }
    if (id == 0x39102) {
        at<uint8_t*>(s, 0x28) = node;
        return 1;
    }
    return 0;
}
// 0x00579C90 (reset): +4 = 1, +5 = 0, +8 = 0, +0x34 = 0. EAX = 0.
uint32_t __cdecl Misc_Reset579C90(uint8_t* s) {
    s[4] = 1;
    s[5] = 0;
    at<uint32_t>(s, 8) = 0;
    at<uint32_t>(s, 0x34) = 0;
    return 0;
}
// 0x00581020 / 0x005819C0 GameFlowManager::LoadTrack / UnloadTrack: set the state (+0xC name, +4 handler), clear
// the busy flag +0x18. EAX = 0.
uint32_t game_flow(uint8_t* s, uint32_t name, uint32_t handler) {
    if (at<uint32_t>(s, 4)) at<uint32_t>(s, 4) = 0;
    at<uint32_t>(s, 0xC) = name;
    at<uint32_t>(s, 4) = handler;
    if (s[0x18]) s[0x18] = 0;
    return 0;
}
uint32_t __cdecl GameFlow_LoadTrack(uint8_t* s) { return game_flow(s, 0x007A0E00, 0x00580EE0); }
uint32_t __cdecl GameFlow_UnloadTrack(uint8_t* s) { return game_flow(s, 0x007A0E10, 0x005817D0); }

// 0x00575FD0 EasterEggs::ClearNonPersistent: every egg (0x34 bytes at [+0xC], count +0x10) whose persistent byte
// +0x31 is 0 gets +0x30 = 0. EAX = the count.
uint32_t __cdecl EasterEggs_ClearNonPersistent(uint8_t* s) {
    uint32_t n = at<uint32_t>(s, 0x10);
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t* e = ptr(s, 0xC) + i * 0x34;
        if (e[0x31] == 0) e[0x30] = 0;
        n = at<uint32_t>(s, 0x10);
    }
    return n;
}

// ---- input / build / loading queries -------------------------------------------------------------------------

// 0x005719C0 QueuedJoystickEvents::RemoveEvent(code, value): clear (word +0 of each 12-byte event at +4) every
// event with byte +2 == code and byte +3 == value. EAX = the end pointer (or the count if <= 0).
uint32_t __cdecl JoyEvents_RemoveEvent(uint8_t* s, uint32_t code, uint32_t val) {
    const int32_t n0 = at<int32_t>(s, 0);
    if (n0 <= 0) return static_cast<uint32_t>(n0);
    int32_t i = 0;
    do {
        uint8_t* e = s + 4 + i * 12;
        if (e[2] == code && e[3] == static_cast<uint8_t>(val)) at<uint16_t>(e, 0) = 0;
        ++i;
    } while (i < at<int32_t>(s, 0));
    return u(s) + 7 + static_cast<uint32_t>(i) * 12;
}
// 0x005004B0 ReInitJoystickTypeMonitoring(bool): +0x7138 = it. AL = it, upper EAX untouched.
uint32_t __cdecl ReInitJoystickTypeMonitoring(uint8_t* s, uint32_t b, uint32_t eax_in) {
    s[0x7138] = static_cast<uint8_t>(b);
    return (eax_in & 0xFFFFFF00u) | static_cast<uint8_t>(b);
}
// 0x00571A80 IsLiveJoystickPluggedIn(port): its slot (live table 0x8647A0 or the other 0x864788) != 0xFF
uint32_t __cdecl IsLiveJoystickPluggedIn(uint32_t port) {
    const uint8_t v = *reinterpret_cast<const uint8_t*>((*reinterpret_cast<const uint8_t*>(0x008650C0u) ? 0x008647A0u : 0x00864788u) + port);
    return v != 0xFF ? 1u : 0u;
}
// 0x00570A40 IsBuildRegionEurope: region 0x864F24 is 1 or 5..9. AL = result, upper EAX = the region.
uint32_t __cdecl IsBuildRegionEurope() {
    const uint32_t r = gu(0x00864F24u);
    const bool e = r == 1 || (static_cast<int32_t>(r) > 4 && static_cast<int32_t>(r) <= 9);
    return (r & 0xFFFFFF00u) | (e ? 1u : 0u);
}
// 0x00571060 IsResourceLoadingComplete: pending count 0x864F54 == 0
uint32_t __cdecl IsResourceLoadingComplete() { return gu(0x00864F54u) == 0 ? 1u : 0u; }
// 0x00571090 IsCurrentlyHotChunking: 0x864F7C != 0
uint32_t __cdecl IsCurrentlyHotChunking() { return gu(0x00864F7Cu) != 0 ? 1u : 0u; }

}  // namespace

LEAF_THIS(0x00574830, Cubic1D_MakeCoeffs, 0, "void tCubic1D::MakeCoeffs()");
LEAF_THIS(0x00574890, Cubic1D_GetddVal, 1, "float tCubic1D::GetddVal(float t)");
LEAF_THIS(0x00574B10, Cubic2D_SetVal, 1, "void tCubic2D::SetVal(const bVector2*)");
LEAF_THIS(0x00574B50, Cubic2D_SetdVal, 1, "void tCubic2D::SetdVal(const bVector2*)");
LEAF_THIS(0x00574B90, Cubic2D_SetValDesired, 1, "void tCubic2D::SetValDesired(const bVector2*)");
LEAF_THIS(0x00574BD0, Cubic2D_SetdValDesired, 1, "void tCubic2D::SetdValDesired(const bVector2*)");
LEAF_THIS(0x00574CF0, Cubic3D_SetdValDesired, 1, "void tCubic3D::SetdValDesired(const bVector3*)");
LEAF_THIS(0x00574BF0, Cubic2D_GetVal, 1, "void tCubic2D::GetVal(bVector2*)");
LEAF_THIS(0x00574C10, Cubic2D_GetdVal, 1, "void tCubic2D::GetdVal(bVector2*)");
LEAF_THIS(0x00574C70, Cubic2D_GetValDesired, 1, "void tCubic2D::GetValDesired(bVector2*)");
LEAF_THIS(0x00574DD0, Cubic3D_GetValDesired, 1, "void tCubic3D::GetValDesired(bVector3*)");
LEAF_THIS(0x00574C30, Cubic2D_GetValAt, 2, "void tCubic2D::GetVal(bVector2*, float t)");
LEAF_THIS(0x00574D70, Cubic3D_GetValAt, 2, "void tCubic3D::GetVal(bVector3*, float t)");
LEAF_THIS_EAX(0x00576C60, Linear_Init, 4, "void Linear::Init(float x0, float y0, float x1, float y1)");
LEAF_THIS(0x00576C90, Linear_GetValue, 1, "float Linear::GetValue(float x)");
LEAF_THIS(0x00576910, Average_Recalculate, 0, "void Average::Recalculate()");
LEAF_THIS(0x005769D0, PidError_ResetIntegral, 1, "void PidError::ResetIntegral(float)");
LEAF_THIS(0x00576770, Graph_ctor, 2, "Graph::Graph(const Point*, int)");
LEAF_CDECL(0x00576CE0, RealTimeFromQuantized, 1, "float GetRealTimeElapsedFromQuantized(int)");
LEAF_THIS(0x005771B0, Timer_GetHMS, 4, "void Timer::GetHoursMinsSeconds(int*, int*, int*, int*)");
LEAF_THIS(0x00574FD0, Repeat_ReadFromLocation, 2, "int bRepeat::ReadFromLocation(int bitpos, int nbits)");
LEAF_CDECL(0x005755E0, FileSize, 1, "int bFileSize(bFile*)");
LEAF_THIS(0x006F6C23, DeviceDriver_ctor, 1, "RealFile::DeviceDriver::DeviceDriver(const char* name)");
LEAF_THIS(0x00571830, Replay_MarkTheEndOfTime, 0, "void ReplayManager::MarkTheEndOfTime()");
LEAF_THIS(0x00571850, Replay_WasSnapshotTaken, 1, "bool ReplayManager::WasSnapshotTakenAtThisTime(int)");
LEAF_THIS(0x00579020, Replay_GetSnapshot, 1, "ReplaySnapshot* ReplayManager::GetSnapshot(int)");
LEAF_THIS(0x00579050, Replay_DeleteSnapshot, 1, "void ReplayManager::DeleteSnapshot(ReplaySnapshot*)");
LEAF_THIS(0x00579260, DemoReplay_Loader, 1, "int DemoModeReplay::Loader(bChunk*)");
LEAF_THIS(0x00579C90, Misc_Reset579C90, 0, "void (reset) 0x579C90");
LEAF_THIS(0x00581020, GameFlow_LoadTrack, 0, "void GameFlowManager::LoadTrack()");
LEAF_THIS(0x005819C0, GameFlow_UnloadTrack, 0, "void GameFlowManager::UnloadTrack()");
LEAF_THIS(0x00575FD0, EasterEggs_ClearNonPersistent, 0, "void EasterEggs::ClearNonPersistent()");
LEAF_THIS(0x005719C0, JoyEvents_RemoveEvent, 2, "void QueuedJoystickEvents::RemoveEvent(int, int)");
LEAF_THIS_EAX(0x005004B0, ReInitJoystickTypeMonitoring, 1, "void ReInitJoystickTypeMonitoring(bool)");
LEAF_CDECL(0x00571A80, IsLiveJoystickPluggedIn, 1, "bool IsLiveJoystickPluggedIn(int port)");
LEAF_CDECL(0x00570A40, IsBuildRegionEurope, 0, "bool IsBuildRegionEurope()");
LEAF_CDECL(0x00571060, IsResourceLoadingComplete, 0, "bool IsResourceLoadingComplete()");
LEAF_CDECL(0x00571090, IsCurrentlyHotChunking, 0, "bool IsCurrentlyHotChunking()");

NFSU2_DIFF_TEST(zmisc_leaf1) {
    // 0 this; 1 sample array (+0x10); 2 snapshot node (list head +0x98); 3 free-list tail (+0x94); 4 node to delete
    // (5 next, 6 prev); 7 demo list tail (+0x30); 8 chunk; 9-12 buffers / names
    static const Spec spec{13, 0x400,
                           {{0, 0x10, 1}, {0, 0x98, 2}, {2, 0, 0, false, 0x98}, {0, 0x94, 3}, {4, 0, 5}, {4, 4, 6},
                            {0, 0x30, 7}},
                           {{0, 5, 1, 16}, {0, 6, 1, 4}, {0, 0xD4, 4, 16}, {0, 0xD8, 4, 16}, {8, 0, 4, 4, 0x390FF},
                            {0, 0xE0, 4, 4}, {0, 0xE4, 4, 4}, {0, 0xE8, 4, 4}, {0, 0xEC, 4, 4}, {0, 0xF0, 4, 4},
                            {0, 0xF4, 4, 4}, {0, 0xF8, 4, 4}, {0, 0xFC, 4, 4}, {0, 0x100, 4, 4}, {0, 0x104, 4, 4},
                            {0, 0x108, 4, 4}, {0, 0x10C, 4, 4}, {0, 0x110, 4, 4}, {0, 0x114, 4, 4}, {0, 0x118, 4, 4},
                            {0, 0x11C, 4, 4}}};
    static const std::vector<Fn> fns{
        LEAF_FN(Cubic2D_SetdValDesired, THIS, INT, {'p', 9}),
        LEAF_FN(Cubic3D_SetdValDesired, THIS, INT, {'p', 9}),
        LEAF_FN(Cubic2D_GetVal, THIS, PTR, {'p', 9}),
        LEAF_FN(Cubic2D_GetdVal, THIS, PTR, {'p', 9}),
        LEAF_FN(Cubic2D_GetValDesired, THIS, PTR, {'p', 9}),
        LEAF_FN(Cubic3D_GetValDesired, THIS, PTR, {'p', 9}),
        LEAF_FN(Linear_Init, THIS, INT, {'f', 0}, {'f', 0}, {'f', 0}, {'f', 0}),
        LEAF_FN(Linear_GetValue, THIS, FLT, {'f', 0}),
        LEAF_FN(Graph_ctor, THIS, PTR, {'r', 0}, {'r', 0}),
        LEAF_FN(RealTimeFromQuantized, CDECL, FLT, {'r', 0}),
        LEAF_FN(Timer_GetHMS, THIS, INT, {'p', 9}, {'p', 10}, {'p', 11}, {'p', 12}),
        LEAF_FN(FileSize, CDECL, INT, {'p', 9}),
        LEAF_FN(Replay_MarkTheEndOfTime, THIS, INT),
        LEAF_FN(Replay_WasSnapshotTaken, THIS, INT, {'i', 4}),
        LEAF_FN(Replay_GetSnapshot, THIS, PTR, {'i', 3}),
        LEAF_FN(Replay_DeleteSnapshot, THIS, PTR, {'p', 4}),
        LEAF_FN(DemoReplay_Loader, THIS, INT, {'p', 8}),
        LEAF_FN(Misc_Reset579C90, THIS, INT),
        LEAF_FN(GameFlow_LoadTrack, THIS, INT),
        LEAF_FN(GameFlow_UnloadTrack, THIS, INT),
        LEAF_FN(IsLiveJoystickPluggedIn, CDECL, INT, {'i', 8}),
        LEAF_FN(IsBuildRegionEurope, CDECL, INT),
        LEAF_FN(IsResourceLoadingComplete, CDECL, INT),
        LEAF_FN(IsCurrentlyHotChunking, CDECL, INT),
    };
    return run(ctx, "zmisc_leaf1", spec, fns);
}

NFSU2_DIFF_TEST(zmisc_leaf1_plain) {  // functions that read raw floats/bits all over this: no pointer fields
    static const Spec spec{2, 0x400, {}, {}};
    static const std::vector<Fn> fns{
        LEAF_FN(Cubic1D_MakeCoeffs, THIS, INT),
        LEAF_FN(Cubic1D_GetddVal, THIS, FLT, {'f', 0}),
        LEAF_FN(Cubic2D_GetValAt, THIS, PTR, {'p', 1}, {'f', 0}),
        LEAF_FN(Cubic3D_GetValAt, THIS, PTR, {'p', 1}, {'f', 0}),
        LEAF_FN(Repeat_ReadFromLocation, THIS, INT, {'i', 2048}, {'i', 34}),
        LEAF_FN(Cubic2D_SetVal, THIS, LOW16, {'p', 1}),
        LEAF_FN(Cubic2D_SetdVal, THIS, LOW16, {'p', 1}),
        LEAF_FN(Cubic2D_SetValDesired, THIS, LOW16, {'p', 1}),
    };
    return run(ctx, "zmisc_leaf1_plain", spec, fns);
}

NFSU2_DIFF_TEST(zmisc_leaf1_avg) {  // samples at [+0x10], count +5, filled +6
    static const Spec spec{2, 0x100, {{0, 0x10, 1}}, {{0, 5, 1, 16}, {0, 6, 1, 4}}};
    static const std::vector<Fn> fns{LEAF_FN(Average_Recalculate, THIS, INT), LEAF_FN(PidError_ResetIntegral, THIS, INT, {'f', 0})};
    return run(ctx, "zmisc_leaf1_avg", spec, fns);
}

NFSU2_DIFF_TEST(zmisc_leaf1_dev) {  // name copied to +4: no pointer fields in this
    static const Spec spec{2, 0x400, {}, {}};
    static const std::vector<Fn> fns{LEAF_FN(DeviceDriver_ctor, THIS, PTR, {'p', 1})};
    return run(ctx, "zmisc_leaf1_dev", spec, fns);
}

NFSU2_DIFF_TEST(zmisc_leaf1_eggs) {  // egg array at [+0xC], count +0x10
    static const Spec spec{2, 0x400, {{0, 0xC, 1}}, {{0, 0x10, 4, 16}}};
    static const std::vector<Fn> fns{LEAF_FN(EasterEggs_ClearNonPersistent, THIS, INT)};
    return run(ctx, "zmisc_leaf1_eggs", spec, fns);
}

NFSU2_DIFF_TEST(zmisc_leaf1_joy) {  // event count +0, 12-byte events at +4 (code +2, value +3); +0x7138 flag
    static const Spec spec = [] {
        Spec sp{1, 0x7200, {}, {{0, 0, 4, 12}}};
        for (uint32_t i = 0; i < 12; ++i) sp.ints.push_back({0, 6 + i * 12, 1, 4}), sp.ints.push_back({0, 7 + i * 12, 1, 4});
        return sp;
    }();
    static const std::vector<Fn> fns{LEAF_FN(JoyEvents_RemoveEvent, THIS, INT, {'i', 4}, {'i', 4}),
                                     LEAF_FN(ReInitJoystickTypeMonitoring, THIS, INT, {'r', 0})};
    return run(ctx, "zmisc_leaf1_joy", spec, fns);
}

NFSU2_DIFF_TEST(zmisc_leaf1_BROKEN) {  // harness self-check: tCubic2D::GetVal compared with GetdVal
    static const Spec spec{2, 0x400, {}, {}};
    static const std::vector<Fn> fns{Fn("GetVal_vs_GetdVal", &o_Cubic2D_GetVal, NFSU2_ENTRY(Cubic2D_GetdVal), THIS, PTR, {'p', 1})};
    return run(ctx, "zmisc_leaf1_BROKEN", spec, fns);
}

// Live check specs (game/framework/live.h): `this` bytes, then bytes behind each pointer argument (0 = a value).
LEAF_LIVE(0x00574830, Cubic1D_MakeCoeffs, THIS, INT, 0x30);
LEAF_LIVE(0x00574890, Cubic1D_GetddVal, THIS, FLT, 0x30, 0);
LEAF_LIVE(0x00574B10, Cubic2D_SetVal, THIS, LOW16, 0x60, 8);
LEAF_LIVE(0x00574B50, Cubic2D_SetdVal, THIS, LOW16, 0x60, 8);
LEAF_LIVE(0x00574B90, Cubic2D_SetValDesired, THIS, LOW16, 0x60, 8);
LEAF_LIVE(0x00574BD0, Cubic2D_SetdValDesired, THIS, INT, 0x60, 8);
LEAF_LIVE(0x00574CF0, Cubic3D_SetdValDesired, THIS, INT, 0x90, 12);
LEAF_LIVE(0x00574BF0, Cubic2D_GetVal, THIS, PTR, 0x60, 8);
LEAF_LIVE(0x00574C10, Cubic2D_GetdVal, THIS, PTR, 0x60, 8);
LEAF_LIVE(0x00574C70, Cubic2D_GetValDesired, THIS, PTR, 0x60, 8);
LEAF_LIVE(0x00574DD0, Cubic3D_GetValDesired, THIS, PTR, 0x90, 12);
LEAF_LIVE(0x00574C30, Cubic2D_GetValAt, THIS, PTR, 0x60, 8, 0);
LEAF_LIVE(0x00574D70, Cubic3D_GetValAt, THIS, PTR, 0x90, 12, 0);
LEAF_LIVE(0x00576C60, Linear_Init, THIS, INT, 8, 0, 0, 0, 0);
LEAF_LIVE(0x00576C90, Linear_GetValue, THIS, FLT, 8, 0);
LEAF_LIVE(0x00576910, Average_Recalculate, THIS, INT, 0x20);
LEAF_LIVE(0x005771B0, Timer_GetHMS, THIS, INT, 4, 4, 4, 4, 4);
LEAF_LIVE(0x00576CE0, RealTimeFromQuantized, CDECL, FLT, 0, 0);
