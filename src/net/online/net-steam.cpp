#include "net-steam.hpp"
#include "log.hpp"

#include <windows.h>
#include <cstring>

namespace net::steam {

    namespace {
        using fn_Iface        = void* (*)();
        using fn_GetName      = const char* (*)(void*);
        using fn_GetSteamID   = uint64_t (*)(void*);
        using fn_FriendCount  = int (*)(void*, int);
        using fn_FriendByIdx  = uint64_t (*)(void*, int, int);
        using fn_FriendGame   = bool (*)(void*, uint64_t, void*);
        using fn_FriendName   = const char* (*)(void*, uint64_t);
        using fn_AppId        = uint32_t (*)(void*);
        using fn_IdentSet     = void (*)(void*, uint64_t);
        using fn_IdentGet     = uint64_t (*)(const void*);
        using fn_SendToUser   = int (*)(void*, const void*, const void*, uint32_t, int, int);
        using fn_RecvOnChan   = int (*)(void*, int, void**, int);
        using fn_CloseSession = bool (*)(void*, const void*);
        using fn_MsgRelease   = void (*)(void*);
        using fn_InitRelay    = void (*)(void*);

        struct Api {
            bool tried = false, ok = false;
            void* friends = nullptr; void* user = nullptr; void* msgs = nullptr; void* utils = nullptr;
            fn_GetName GetPersonaName = nullptr;
            fn_GetSteamID GetSteamID = nullptr;
            fn_FriendCount GetFriendCount = nullptr;
            fn_FriendByIdx GetFriendByIndex = nullptr;
            fn_FriendGame GetFriendGamePlayed = nullptr;
            fn_FriendName GetFriendPersonaName = nullptr;
            fn_AppId GetAppID = nullptr;
            fn_IdentSet IdentSet = nullptr;
            fn_IdentGet IdentGet = nullptr;
            fn_SendToUser SendToUser = nullptr;
            fn_RecvOnChan RecvOnChannel = nullptr;
            fn_CloseSession CloseSession = nullptr;
            fn_MsgRelease MsgRelease = nullptr;
            uint32_t appId = 0;
        };
        Api g;

        // SteamNetworkingIdentity is a 136-byte blob; SetSteamID64 fills it.
        struct Ident { uint8_t raw[136]; };
        Ident MakeIdent(uint64_t id) { Ident i{}; g.IdentSet(i.raw, id); return i; }

        // FriendGameInfo_t: CGameID (u64, appid in the low 24 bits), ip u32, game port u16,
        // query port u16, lobby u64.
        struct FriendGameInfo { uint64_t gameId; uint32_t ip; uint16_t gamePort, queryPort; uint64_t lobby; };

        constexpr int kFriendFlagImmediate = 0x04;
        constexpr int kSendFlags = 1 | 32;          // UnreliableNoNagle | AutoRestartBrokenSession

        template <typename T> T Sym(HMODULE m, const char* n) { return reinterpret_cast<T>(GetProcAddress(m, n)); }

        bool Init()
        {
            if (g.tried) return g.ok;
            g.tried = true;
            HMODULE m = GetModuleHandleA("steam_api64.dll");
            if (!m) { TO1_LOG("[steam] steam_api64.dll is not loaded\n"); return false; }

            auto Friends = Sym<fn_Iface>(m, "SteamAPI_SteamFriends_v017");
            auto User    = Sym<fn_Iface>(m, "SteamAPI_SteamUser_v023");
            auto Msgs    = Sym<fn_Iface>(m, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
            auto Utils   = Sym<fn_Iface>(m, "SteamAPI_SteamUtils_v010");
            auto NUtils  = Sym<fn_Iface>(m, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
            auto InitRelay = Sym<fn_InitRelay>(m, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
            g.GetPersonaName      = Sym<fn_GetName>(m, "SteamAPI_ISteamFriends_GetPersonaName");
            g.GetSteamID          = Sym<fn_GetSteamID>(m, "SteamAPI_ISteamUser_GetSteamID");
            g.GetFriendCount      = Sym<fn_FriendCount>(m, "SteamAPI_ISteamFriends_GetFriendCount");
            g.GetFriendByIndex    = Sym<fn_FriendByIdx>(m, "SteamAPI_ISteamFriends_GetFriendByIndex");
            g.GetFriendGamePlayed = Sym<fn_FriendGame>(m, "SteamAPI_ISteamFriends_GetFriendGamePlayed");
            g.GetFriendPersonaName= Sym<fn_FriendName>(m, "SteamAPI_ISteamFriends_GetFriendPersonaName");
            g.GetAppID            = Sym<fn_AppId>(m, "SteamAPI_ISteamUtils_GetAppID");
            g.IdentSet            = Sym<fn_IdentSet>(m, "SteamAPI_SteamNetworkingIdentity_SetSteamID64");
            g.IdentGet            = Sym<fn_IdentGet>(m, "SteamAPI_SteamNetworkingIdentity_GetSteamID64");
            g.SendToUser          = Sym<fn_SendToUser>(m, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
            g.RecvOnChannel       = Sym<fn_RecvOnChan>(m, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
            g.CloseSession        = Sym<fn_CloseSession>(m, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser");
            g.MsgRelease          = Sym<fn_MsgRelease>(m, "SteamAPI_SteamNetworkingMessage_t_Release");

            if (!Friends || !User || !Msgs || !Utils || !g.GetPersonaName || !g.GetSteamID || !g.GetFriendCount ||
                !g.GetFriendByIndex || !g.GetFriendGamePlayed || !g.GetFriendPersonaName || !g.GetAppID ||
                !g.IdentSet || !g.IdentGet || !g.SendToUser || !g.RecvOnChannel || !g.MsgRelease) {
                TO1_LOG("[steam] missing steam_api64 exports\n");
                return false;
            }
            g.friends = Friends(); g.user = User(); g.msgs = Msgs(); g.utils = Utils();
            if (!g.friends || !g.user || !g.msgs || !g.utils) {
                TO1_LOG("[steam] Steam interfaces unavailable (friends=%p user=%p msgs=%p utils=%p)\n",
                    g.friends, g.user, g.msgs, g.utils);
                return false;
            }
            g.appId = g.GetAppID(g.utils);
            if (NUtils && InitRelay) if (void* nu = NUtils()) InitRelay(nu);   // warm up Valve relay routes
            g.ok = true;
            TO1_LOG("[steam] ready: appid %u, me %llu\n", g.appId, static_cast<unsigned long long>(g.GetSteamID(g.user)));
            return true;
        }
    }

    bool Available() { return Init(); }
    uint64_t MyId() { return Init() ? g.GetSteamID(g.user) : 0; }
    const char* MyName() { return Init() ? g.GetPersonaName(g.friends) : ""; }
    const char* FriendName(uint64_t id) { return Init() ? g.GetFriendPersonaName(g.friends, id) : ""; }

    void FriendsInThisGame(std::vector<uint64_t>& out)
    {
        if (!Init()) return;
        const int n = g.GetFriendCount(g.friends, kFriendFlagImmediate);
        for (int i = 0; i < n && i < 2000; ++i) {
            const uint64_t id = g.GetFriendByIndex(g.friends, i, kFriendFlagImmediate);
            FriendGameInfo info{};
            if (id && g.GetFriendGamePlayed(g.friends, id, &info) && (info.gameId & 0xFFFFFF) == g.appId)
                out.push_back(id);
        }
    }

    bool Send(uint64_t to, const void* data, int len, int channel)
    {
        if (!Init() || !to || len < 0) return false;
        const Ident id = MakeIdent(to);
        return g.SendToUser(g.msgs, id.raw, data, static_cast<uint32_t>(len), kSendFlags, channel) == 1;   // k_EResultOK
    }

    int Recv(int channel, void* buf, int cap, uint64_t* from)
    {
        if (!Init()) return -1;
        void* msg = nullptr;
        if (g.RecvOnChannel(g.msgs, channel, &msg, 1) != 1 || !msg) return -1;
        const uint8_t* m = static_cast<const uint8_t*>(msg);
        const void* data = *reinterpret_cast<const void* const*>(m + 0x00);    // SteamNetworkingMessage_t::m_pData
        int len = *reinterpret_cast<const int*>(m + 0x08);                      // m_cbSize
        if (from) *from = g.IdentGet(m + 0x10);                                 // m_identityPeer
        if (len > cap) len = cap;
        if (len > 0 && data) memcpy(buf, data, static_cast<size_t>(len));
        g.MsgRelease(msg);
        return len;
    }

    void Close(uint64_t peer)
    {
        if (!Init() || !peer || !g.CloseSession) return;
        const Ident id = MakeIdent(peer);
        g.CloseSession(g.msgs, id.raw);
    }
}
