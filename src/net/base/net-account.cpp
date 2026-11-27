// Co-op build: no Twelve Weeks account. Twelve Weeks read a login token from tlou-ii-mp.json
// for its matchmaking server; the co-op mode has no server, so only the display name is kept
// (set from tlou2-coop.ini / the Steam name by the session).
#include "net-account.hpp"

#include <cstring>

namespace net::account {

    namespace { char g_name[64] = { 0 }; }

    const char* Token() { return ""; }
    bool        HaveToken() { return false; }
    const char* Name() { return g_name; }

    void SetName(const char* name)
    {
        if (!name) return;
        strncpy_s(g_name, name, _TRUNCATE);
    }
}
