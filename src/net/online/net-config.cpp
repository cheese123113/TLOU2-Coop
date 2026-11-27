// Co-op build: Twelve Weeks fetched its map list and relay servers from its matchmaking server
// (an authenticated TCP request). The co-op mode plays the campaign and connects players over
// Steam or LAN, so there is no server and nothing to fetch: no maps, no relays.
#include "net-config.hpp"

namespace net::config {

    void  Begin() {}
    void  Tick() {}
    void  Reset() {}
    State GetState() { return State::Idle; }
    bool  VersionRejected() { return false; }
    bool  TokenRejected() { return false; }

    int MapCount() { return 0; }
    MapInfo Map(int) { return MapInfo{ "", "", "", 0 }; }

    int       RelayCount() { return 0; }
    RelayInfo Relay(int) { return RelayInfo{ "", 0 }; }
}
