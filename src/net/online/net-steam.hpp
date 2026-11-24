#pragma once

#include <cstdint>
#include <vector>

// Steam peer-to-peer for co-op, through the game's own steam_api64.dll (the game has already
// initialised Steam; nothing extra is installed or launched). ISteamNetworkingMessages picks a
// direct connection or a Valve relay, so nobody has to forward ports or know an IP address.
//
// A session with a user is opened implicitly by sending to them, so both sides "ping" the
// other first (see coop.cpp): the host pings every friend who is in this game, a joiner pings
// back, and from then on the two exchange the normal net protocol on channel 0.
namespace net::steam {

    bool Available();                     // exports resolved and the interfaces exist (lazy init)
    uint64_t MyId();
    const char* MyName();
    const char* FriendName(uint64_t id);

    // Friends (immediate friends list) currently playing this same app.
    void FriendsInThisGame(std::vector<uint64_t>& out);

    // Unreliable, no Nagle, auto-restart broken sessions. Returns true when Steam accepted it.
    bool Send(uint64_t to, const void* data, int len, int channel);
    // One message from `channel`; returns its length (copied up to cap) or -1 if none.
    int Recv(int channel, void* buf, int cap, uint64_t* from);
    void Close(uint64_t peer);

    constexpr int kChannelGame = 0;       // net protocol packets
    constexpr int kChannelLobby = 1;      // co-op discovery: "anyone hosting?" / "I am hosting"
}
