// Co-op build: Twelve Weeks' multiplayer front end (main menu, lobby, scoreboard, death banner,
// loadout pages, "open the website" button) is gone. The co-op mode never shows it; the PvP
// code that still compiles against this interface gets inert answers: no match, no menu,
// default HUD modes, no weapons picked.
#include "net-gui2.hpp"

namespace net::gui2 {

    StringId64 w_scoreBanner = 0;
    StringId64 w_deathBanner = 0;

    ReticleMode GetReticleMode() { return ReticleMode::MovingReticle; }
    RadarMode   GetRadarMode() { return RadarMode::Square; }

    void Spawn() {}
    void Despawn() {}
    void RefreshWidgets() {}
    bool ScoreboardShowing() { return false; }
    void BeginMatchOver(int) {}
    void ResetMultiplayer() {}
    void LeaveMatch() {}
    void ResetHudLatches() {}

    void SetLives(int, int) {}
    int  GetBlueLives() { return 0; }
    int  GetRedLives() { return 0; }
    void DecrementLivesForTeam(int) {}
    int  GetLivesForTeam(int) { return 0; }
    void SetLivesAbsolute(int, int) {}
    void RecordKill(int, int, bool) {}

    void         StartMatch() {}
    void         ClearMatch() {}
    void         TickMatch(float) {}
    bool         MatchRunning() { return false; }
    bool         MatchExpired() { return false; }
    NetTimerSync MatchSnapshot() { return NetTimerSync{}; }
    void         ApplyMatchSync(const NetTimerSync&) {}
    bool         InMatch() { return false; }
    bool         PauseMenuOpen() { return false; }

    void SetDeathBannerKiller(const char*, const char*) {}
    void SetDeathBannerRespawn(int) {}
    void SetDeathBannerVisible(bool) {}

    void       UpdateMenu() {}
    StringId64 PrimaryWeaponSid() { return 0; }
    StringId64 SecondaryWeaponSid() { return 0; }
    int  PrimaryWeaponStartingAmmo() { return 0; }
    int  SecondaryWeaponStartingAmmo() { return 0; }
    int  PurchasableWeaponStartingAmmo() { return 0; }
}
