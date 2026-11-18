// Functions many camera functions call (camera batch 3, 8), written from the PC machine code: point transform,
// view table lookup, car POV lookup, tCubic3D value getter, CAnimPlayer / EAXSound object lookups (circular
// intrusive lists, link at node +0x10), matrix copy / build from rows + translation.
#include "leaf.h"
#include "live.h"
#include "x87.h"

namespace {
using namespace x87;

// 0x00441000 (transform a point): out = M * v + translation (rows +0x00/+0x10/+0x20 times v, plus +0x30).
// All three components are computed before any is stored. EAX = out.
uint32_t __cdecl TransformPoint(uint32_t op, uint32_t mp, uint32_t vp) {
    const float* m = reinterpret_cast<const float*>(mp);
    const float* v = reinterpret_cast<const float*>(vp);
    const X vx = ld(v[0]), vy = ld(v[1]), vz = ld(v[2]);
    const X x = add(add(add(mul(ld(m[8]), vz), mul(ld(m[4]), vy)), mul(ld(m[0]), vx)), ld(m[12]));
    const X y = add(add(add(mul(ld(m[9]), vz), mul(ld(m[1]), vx)), mul(ld(m[5]), vy)), ld(m[13]));
    const X z = add(add(add(mul(ld(m[10]), vz), mul(ld(m[2]), vx)), mul(ld(m[6]), vy)), ld(m[14]));
    float* out = reinterpret_cast<float*>(op);
    out[0] = st(x);
    out[1] = st(y);
    out[2] = st(z);
    return op;
}

// 0x0048B1E0 eGetView(i): the view table 0x832DE0, 0x70 bytes per view
uint32_t __cdecl GetView(uint32_t i) { return 0x00832DE0u + i * 0x70u; }

// 0x005ECCA0 Car::GetPov(id): the car's camera info ([+0xC] + 0x720) POV with that id, else POV 0
uint32_t __cdecl Car_GetPov(uint8_t* s, uint32_t id) {
    using GetPov = uint32_t(__thiscall*)(void*, uint32_t);
    uint8_t* info = ptr(s, 0xC) + 0x720;
    const uint32_t r = game_fn<GetPov>(0x00441140)(info, id);
    return r ? r : game_fn<GetPov>(0x00441140)(info, 0);
}

// 0x00574D30 (tCubic3D value): out = (+0x00, +0x30, +0x60). EAX = out.
uint32_t __cdecl Cubic3D_GetVal(uint8_t* s, uint32_t op) {
    uint32_t* out = reinterpret_cast<uint32_t*>(op);
    out[0] = at<uint32_t>(s, 0);
    out[1] = at<uint32_t>(s, 0x30);
    out[2] = at<uint32_t>(s, 0x60);
    return op;
}

// intrusive list: head link at this+4, a node's link at node+0x10 (so link - 0x10 = node; the head's "node" is this-0xC)
inline uint8_t* node_of(uint32_t link) { return link ? reinterpret_cast<uint8_t*>(link - 0x10) : nullptr; }
// 0x004324D0 CAnimPlayer::FindHandle(h): the first node's handle (+0x18), 0 for an empty list (the argument is not
// used on PC)
uint32_t __cdecl AnimPlayer_FindHandle(uint8_t* s, uint32_t) {
    uint8_t* first = node_of(at<uint32_t>(s, 4));
    uint8_t* end = node_of(u(s) + 4);
    return first == end ? 0u : at<uint32_t>(first, 0x18);
}
// 0x00432490 EAXSound::GetSFXBase_Object(id): the node with id +0x18
uint32_t __cdecl EAXSound_GetSFXBase(uint8_t* s, uint32_t id) {
    uint8_t* e = node_of(at<uint32_t>(s, 4));
    uint8_t* end = node_of(u(s) + 4);
    for (;;) {
        if (e == end) return 0;
        if (at<uint32_t>(e, 0x18) == id) return u(e);
        e = node_of(at<uint32_t>(e, 0x10));
    }
}

// matrix rows: dwords 0 and 3 copied raw, 1 and 2 through the x87 (fld/fstp) like the original
void copy_row(uint8_t* out, const uint8_t* src) {
    const X a = ld(at<float>(src, 4)), b = ld(at<float>(src, 8));
    at<uint32_t>(out, 0) = at<uint32_t>(src, 0);
    at<float>(out, 4) = st(a);
    at<float>(out, 8) = st(b);
    at<uint32_t>(out, 0xC) = at<uint32_t>(src, 0xC);
}
// 0x004012B0 (copy a 4x4 matrix). EAX = out.
uint32_t __cdecl CopyMatrix(uint32_t op, uint32_t sp) {
    for (uint32_t r = 0; r < 4; ++r) copy_row(reinterpret_cast<uint8_t*>(op) + r * 16, reinterpret_cast<uint8_t*>(sp) + r * 16);
    return op;
}
// 0x00401340 (matrix from a rotation and a position): rows 0-2 copied, row 3 = (pos.x, pos.y, pos.z, 1). EAX = out.
uint32_t __cdecl MatrixFromRotPos(uint32_t op, uint32_t sp, uint32_t pp) {
    uint8_t* out = reinterpret_cast<uint8_t*>(op);
    for (uint32_t r = 0; r < 3; ++r) copy_row(out + r * 16, reinterpret_cast<uint8_t*>(sp) + r * 16);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(pp);
    const X y = ld(at<float>(p, 4)), z = ld(at<float>(p, 8));
    at<uint32_t>(out, 0x30) = at<uint32_t>(p, 0);
    at<float>(out, 0x34) = st(y);
    at<uint32_t>(out, 0x3C) = 0x3F800000u;
    at<float>(out, 0x38) = st(z);
    return op;
}

}  // namespace

LEAF_CDECL(0x00441000, TransformPoint, 3, "bVector3* TransformPoint(bVector3* out, const bMatrix4*, const bVector3*)");
LEAF_CDECL(0x0048B1E0, GetView, 1, "eView* eGetView(int)");
LEAF_THIS(0x005ECCA0, Car_GetPov, 1, "CameraPov* Car::GetPov(int id)");
LEAF_THIS(0x00574D30, Cubic3D_GetVal, 1, "void tCubic3D::GetVal(bVector3* out)");
LEAF_THIS(0x004324D0, AnimPlayer_FindHandle, 1, "int CAnimPlayer::FindHandle(int)");
LEAF_THIS(0x00432490, EAXSound_GetSFXBase, 1, "SFXBase* EAXSound::GetSFXBase_Object(int id)");
LEAF_CDECL(0x004012B0, CopyMatrix, 2, "bMatrix4* CopyMatrix(bMatrix4* out, const bMatrix4*)");
LEAF_CDECL(0x00401340, MatrixFromRotPos, 3, "bMatrix4* MatrixFromRotPos(bMatrix4* out, const bMatrix4* rot, const bVector3* pos)");

NFSU2_DIFF_TEST(camera_deps1) {  // plain data: matrices / vectors / a cubic
    static const Spec spec{4, 0x200, {}, {}};
    static const std::vector<Fn> fns{
        LEAF_FN(TransformPoint, CDECL, PTR, {'p', 1}, {'p', 2}, {'p', 3}),
        LEAF_FN(TransformPoint, CDECL, PTR, {'p', 1}, {'p', 2}, {'p', 1}),  // out == v (all read first)
        LEAF_FN(GetView, CDECL, PTR, {'i', 8}),
        LEAF_FN(Cubic3D_GetVal, THIS, PTR, {'p', 1}),
        LEAF_FN(CopyMatrix, CDECL, PTR, {'p', 1}, {'p', 2}),
        LEAF_FN(MatrixFromRotPos, CDECL, PTR, {'p', 1}, {'p', 2}, {'p', 3}),
    };
    return run(ctx, "camera_deps1", spec, fns);
}

NFSU2_DIFF_TEST(camera_deps1_lists) {
    // 0 owner (head link +4) -> node 1 (+0x10) -> node 2 (+0x10) -> back to the head; ids at node +0x18
    static const Spec spec{3, 0x100, {{0, 4, 1, false, 0x10}, {1, 0x10, 2, false, 0x10}, {2, 0x10, 0, false, 4}},
                           {{1, 0x18, 4, 4}, {2, 0x18, 4, 4}}};
    static const std::vector<Fn> fns{
        LEAF_FN(AnimPlayer_FindHandle, THIS, INT, {'r', 0}),
        LEAF_FN(EAXSound_GetSFXBase, THIS, PTR, {'i', 5}),
    };
    return run(ctx, "camera_deps1_lists", spec, fns);
}

NFSU2_DIFF_TEST(camera_deps1_empty) {  // an empty list: the head link points at itself
    static const Spec spec{1, 0x100, {{0, 4, 0, false, 4}}, {}};
    static const std::vector<Fn> fns{
        LEAF_FN(AnimPlayer_FindHandle, THIS, INT, {'r', 0}),
        LEAF_FN(EAXSound_GetSFXBase, THIS, PTR, {'i', 5}),
    };
    return run(ctx, "camera_deps1_empty", spec, fns);
}

NFSU2_DIFF_TEST(camera_deps1_car) {
    // a car: [+0xC] -> block 1 whose +0x720 is a CameraInfo (counts +0 / +4, POV ids every 16 bytes from +0x10 /
    // +0x70)
    static const Spec spec = [] {
        Spec sp{2, 0x900, {{0, 0xC, 1}}, {{1, 0x720, 4, 6}, {1, 0x724, 4, 6}}};
        for (uint32_t i = 0; i < 6; ++i) sp.ints.push_back({1, 0x730 + i * 16, 2, 4}), sp.ints.push_back({1, 0x790 + i * 16, 2, 4});
        return sp;
    }();
    static const std::vector<Fn> fns{LEAF_FN(Car_GetPov, THIS, PTR, {'i', 4})};
    return run(ctx, "camera_deps1_car", spec, fns);
}

NFSU2_DIFF_TEST(camera_deps1_BROKEN) {  // harness self-check: CopyMatrix compared with MatrixFromRotPos
    static const Spec spec{4, 0x200, {}, {}};
    static const std::vector<Fn> fns{Fn("Copy_vs_FromRotPos", &o_CopyMatrix, NFSU2_ENTRY(MatrixFromRotPos), CDECL, PTR, {'p', 1}, {'p', 2}, {'p', 3})};
    return run(ctx, "camera_deps1_BROKEN", spec, fns);
}

// Live check specs (game/framework/live.h)
LEAF_LIVE(0x00441000, TransformPoint, CDECL, PTR, 0, 12, 0x40, 12);
LEAF_LIVE(0x0048B1E0, GetView, CDECL, PTR, 0, 0);
LEAF_LIVE(0x00574D30, Cubic3D_GetVal, THIS, PTR, 0x90, 12);
LEAF_LIVE(0x004012B0, CopyMatrix, CDECL, PTR, 0, 0x40, 0x40);
LEAF_LIVE(0x00401340, MatrixFromRotPos, CDECL, PTR, 0, 0x40, 0x40, 12);
LEAF_LIVE(0x005ECCA0, Car_GetPov, THIS, PTR, 0x10, 0);
