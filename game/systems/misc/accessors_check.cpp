// Harness self-check for the generated accessors (game/framework/accessors.h): a field getter read 4 bytes off must
// be caught.
#include <vector>

#include "accessors.h"

NFSU2_DIFF_TEST(accessors_BROKEN) {
    const auto& t = ::game::acc::table();
    const void* anchor = nullptr;
    for (const auto& e : t)
        if (e.kind == ::game::acc::GET32 && *e.orig) anchor = *e.orig;
    return game::run_diff(ctx, "accessors_BROKEN", anchor, [](game::Rng& r) -> game::Case {
        const auto& tt = ::game::acc::table();
        for (const auto& e : tt) {
            if (e.kind != ::game::acc::GET32 || !*e.orig) continue;
            std::vector<uint8_t> obj(::game::acc::kObj + 8);
            for (auto& x : obj) x = static_cast<uint8_t>(r.next());
            const uint32_t got = game_fn<uint32_t(__thiscall*)(void*)>(reinterpret_cast<uintptr_t>(*e.orig))(obj.data());
            return got == ::game::acc::fld<uint32_t>(obj.data(), e.off + 4) ? game::Ok() : game::MismatchText("off by 4");
        }
        return game::Ok();
    });
}
