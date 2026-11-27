// Co-op build: no matchmaking server. Twelve Weeks connected to its own server over TCP for
// login (token + hardware id), queues, lobbies, leaderboards and tamper reports. The co-op
// mode joins a Steam friend (or a LAN address) directly, so this module is inert: it never
// connects, never matches and reports nothing.
#include "net-matchmaking.hpp"

namespace net::matchmaking {

    void Connect(const char*) {}
    void FindMatch(const char*, uint8_t, uint8_t, uint64_t, uint64_t) {}
    void Cancel() {}
    void Tick() {}
    State GetState() { return State::Idle; }

    int         LobbyNameCount() { return 0; }
    const char* LobbyName(int) { return ""; }
    // Same answers as Twelve Weeks gave outside a matchmade game (always the case in co-op):
    // no loadout, the match's own team, no team for a slot.
    bool SlotLoadout(int, StringId64&, StringId64&) { return false; }
    int MyTeam() { return net::match::Get().MyTeam(); }
    int SlotTeam(int) { return -1; }

    bool VersionRejected() { return false; }
    bool TokenRejected() { return false; }
    void SendReady() {}
    bool StartReceived() { return false; }
    bool Aborted() { return false; }
    bool TakeBroadcast(char*, int) { return false; }
    void ReportResult(int) {}
    bool Authed() { return false; }
    bool ReportTamper(const MMTamper&) { return false; }

    void                 RequestLeaderboard() {}
    const MMLeaderEntry* LeaderboardEntry(int) { return nullptr; }
    void                 RequestOnlineCount() {}
    uint32_t             OnlineCount() { return 0; }

    int LobbyPhase() { return 0; }
    int LobbyCountdown() { return 0; }

    uint32_t    ServerAddr() { return 0; }
    const char* ServerHost() { return ""; }
    uint16_t    ServerPort() { return 0; }
}
