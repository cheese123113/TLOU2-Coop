#pragma once

#include "../perk.hpp"

namespace gameplay {

    void SteadyHandsTick(float dt);

    inline constexpr Perk SteadyHands = {
        .id = PerkId::STEADYHANDS,
        .name = "STEADY HANDS",
        .description = "Your weapon does not sway while aiming.",
        .icon = "hud/weapon-upgrades/badges/stability.png",
        .lpCost = 5,
        .OnTick = &SteadyHandsTick,
    };
}