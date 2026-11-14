// nfsu2_game.dll: our rewrite of SPEED2.EXE, one function at a time. Loaded and hot-reloaded by the probe core.
#include "rewrite.h"

namespace game {

std::vector<probe::GameRewrite>& registry() {
    static std::vector<probe::GameRewrite> r;
    return r;
}

namespace {
probe::GameApi g_api{};
}  // namespace

}  // namespace game

extern "C" __declspec(dllexport) const probe::GameApi* nfsu2_game_api() {
    auto& r = game::registry();
    game::g_api = {probe::kGameApiVersion, __DATE__ " " __TIME__, static_cast<uint32_t>(r.size()), r.data()};
    return &game::g_api;
}
