#pragma once

#include "net-common.hpp"

namespace net::hooks {

	void InstallMultiplayerHooks();
	void AllowReplayDamage(bool on);
	void InstallMeleeProbe();
	// The victim (process) of the newest melee attack the local player landed, then 0 (consumed).
	uintptr_t TakeLocalMeleeVictim();
	// The weapon / melee class / move of the last real melee attack the local player landed (0s: none yet).
	void LastLocalMelee(uint64_t& weapon, uint32_t& meleeClass, uint64_t& move);       // co-op: log the first real melee attacks the local player lands (layout discovery)  // co-op: let our own replayed hit through the puppet-attacker zeroing
	void InstallCoopHooks();        // co-op: puppet guards only (removed by RemoveMultiplayerHooks)
	void RemoveMultiplayerHooks();

	void ReportWithheldMeleeKill(int netSlot, uint64_t weapon, float damage);
	void ForceLocalDeath(const char* why);
	void SetDeathBlowVictim(uintptr_t victim);

	struct PuppetTeleportScope {
		PuppetTeleportScope();
		~PuppetTeleportScope();
	};
	void EnsureStealthDenyHook();
}