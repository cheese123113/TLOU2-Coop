#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

#ifndef NET_PROTOCOL_HAS_STRINGID
using NetSid64 = uint64_t;
#endif

constexpr uint32_t kNetMagic = 0x4F523254;   // 'T2RO'
constexpr uint16_t kProtocolVersion = 46;          // co-op: 42 NEVT_COOP_ENEMY, 43 CoopEnemies, 44 NEVT_COOP_WHERE, 45 CoopAlive, 46 NEVT_COOP_HURT
constexpr uint16_t kNetDefaultPort = 7777;
constexpr int      kMaxAnimCmdDwords = 1280;
constexpr int      kMaxNetPlayers = 4;
constexpr int      kNetNameLen = 24;           // player display name
constexpr int      kNetLookLen = 40;           // look/model string ("marlene-player")
constexpr uint8_t  kInvalidSlot = 0xFF;
constexpr uint8_t  kBroadcastSlot = 0xFF;         // event target: everyone
constexpr float    kNetTimeoutSeconds = 45.0f;        // silence -> peer dropped (co-op: 45 s -- a joining partner sends nothing during a loading screen)
constexpr float    kHelloResendSeconds = 0.5f;         // client join retry
constexpr float    kJoinGiveUpSeconds = 10.0f;
constexpr float    kEventResendSeconds = 0.1f;         // reliable event retry
constexpr float    kEventExpireSeconds = 5.0f;         // give up resending after
constexpr int      kMaxPerks = 16;

enum class NetPacketType : uint8_t {
    Hello = 1,   // client -> host : "let me in" (HelloPayload)
    Welcome = 2,   // host -> client : "you are slot N" (RosterPayload, yourSlot set)
    Reject = 3,   // host -> client : version mismatch / server full (RejectPayload)
    Roster = 4,   // host -> all    : membership changed (RosterPayload)
    State = 5,   // both ways      : PlayerNetState (+ embedded reliable events)
    Goodbye = 6,   // either way     : clean disconnect
    AnimCmd = 7,   // both ways      : AnimCmdPayload -- the sender's captured AnimCmdList
    CoopEnemies = 8, // host -> all  : CoopEnemiesPayload -- co-op: where the host's enemies are
    CoopAlive = 9,   // host -> all  : CoopAlivePayload -- co-op: every enemy alive on the host (late-join reconcile)
};

enum class NetRejectReason : uint8_t {
    VersionMismatch = 1,
    ServerFull = 2,
};

#pragma pack(push, 1)

struct NetPacketHeader {
    uint32_t magic;        // kNetMagic
    uint16_t version;      // kProtocolVersion
    uint8_t  type;         // NetPacketType
    uint8_t  senderSlot;
    uint32_t sequence;
};
static_assert(sizeof(NetPacketHeader) == 12, "header layout");

struct HelloPayload {
    char name[kNetNameLen];
    char look[kNetLookLen];
};
static_assert(sizeof(HelloPayload) == 64, "HelloPayload layout");

constexpr uint8_t kTeamUnknown = 0xFF;

struct RosterEntry {
    uint8_t slot;
    uint8_t active;                 // 1 = occupied
    uint8_t team;                   // matchmaker team (0=Seraphites, 1=WLF; kTeamUnknown=unresolved).
    // Host-authoritative: the host stamps it from its complete match
    // roster, so no client depends on its own private matchmaker copy.
    char    name[kNetNameLen];
    char    look[kNetLookLen];
};
static_assert(sizeof(RosterEntry) == 67, "RosterEntry layout");

struct RosterPayload {
    uint8_t     yourSlot;           // only meaningful in Welcome; kInvalidSlot otherwise
    uint8_t     count;              // entries used
    RosterEntry players[kMaxNetPlayers];
};
static_assert(sizeof(RosterPayload) == 270, "RosterPayload layout (grows with kMaxNetPlayers)");

struct RejectPayload {
    uint8_t  reason;                // NetRejectReason
    uint16_t hostVersion;
};
static_assert(sizeof(RejectPayload) == 3, "RejectPayload layout");

struct PlayerNetState {
    float posX, posY, posZ;
    float rotX, rotY, rotZ, rotW;
    uint8_t isAiming;
    uint8_t isCrouched;
    uint8_t isProne;
    uint8_t isSprinting;
    uint8_t isJumping;
    uint8_t isFiring;
    uint8_t isReloading;
    uint8_t inMelee;
    uint8_t inDebugFly;
    uint8_t inBackpack;
    uint8_t isMnk;
    // Weapon-IK feather fade, 0..255 == 0..1. The engine's gate for two-handed weapon
    // grip IK: 0 while the weapon is stowed, ramps across draw/holster, 1 with it out.
    // Read off the local player's own arm controller (engine::GetWeaponIkFade) and
    // replayed onto the puppet as the weapon-ik-feather MASTER weight -- the puppet
    // cannot derive it itself (our replay replaces its anim-state layer walk wholesale,
    // so ND's own NPC emitter never runs and the IK is never applied).
    uint8_t weaponIk;
    uint8_t aimSideLeft;
    NetSid64 weaponId;
    NetSid64 meleeAction;
    uint32_t shotCount;
    float    health;
    float camPosX, camPosY, camPosZ;
    float camRotX, camRotY, camRotZ, camRotW;
    float    animSpine[20];
    uint16_t eventBytes;
    uint8_t  eventBuf[1024];
};
static_assert(offsetof(PlayerNetState, eventBuf) == 175, "PlayerNetState fixed-part layout");
static_assert(sizeof(PlayerNetState) == 1199, "PlayerNetState layout");

inline size_t NetStateWireSize(uint16_t eventBytes)
{
    return offsetof(PlayerNetState, eventBuf) + eventBytes;
}


constexpr int kMaxAnimCmdBytes = 4068;
struct AnimCmdPayload {
    uint32_t skelId; // sender's ArtItemSkeleton id (*(u32*)(skel+52))
    uint32_t sendMs;
    uint8_t  dictEpoch;
    uint16_t bits;
    uint8_t  data[kMaxAnimCmdBytes];
};
static_assert(offsetof(AnimCmdPayload, data) == 11, "AnimCmdPayload fixed-part layout");
static_assert(sizeof(AnimCmdPayload) == 4079, "AnimCmdPayload layout");

inline size_t NetAnimCmdWireSize(uint16_t bits)
{
    return offsetof(AnimCmdPayload, data) + ((static_cast<size_t>(bits) + 7u) / 8u);
}

constexpr size_t kMaxPacketSize = sizeof(NetPacketHeader) + sizeof(PlayerNetState);

constexpr size_t kPathMtuPayload = 1472;

enum NetEventType : uint8_t {
    NEVT_OPEN_DOOR,          // DoorOpenEvent
    NEVT_CHAT,               // string
    NEVT_THROWPROJECTILE,    // ProjectileThrowEvent
    NEVT_TAKE_DAMAGE,        // PlayerDamageEvent
    NEVT_PLAYER_DEATH,       // PlayerDeathEvent
    NEVT_PLAYER_MELEE_ATTACK,// melee action sid
    NEVT_MATCH_TIMER_SYNC,   // MatchStateSync
    NEVT_FIRE_SHOT,          // FireShotEvent
    NEVT_MATCH_OVER,         // MatchOverEvent
    NEVT_MARK_PLAYER,        // AimMarkEvent
    NEVT_VOX_LINE,           // VoxLineEvent
    NEVT_RESPAWN_REQUEST,
    NEVT_RESPAWN_GRANT,
    NEVT_PING,
    NEVT_SUSPICIOUS_PROCESS,
    NEVT_DEBUGGING,
    NEVT_PLAYER_EXECUTION,   // ExecutionEvent -- the move is starting, play your half
    NEVT_EXECUTION_REQUEST,  // ExecutionRequestEvent -- may I execute you?
    NEVT_EXECUTION_REPLY,    // ExecutionReplyEvent   -- yes / no
    NEVT_COOP_ENEMY,         // CoopEnemyEntry[n]     -- co-op: enemy health / death changes
    NEVT_COOP_WHERE,         // CoopWhereEvent        -- co-op: host -> partner, "this is where I am"
    NEVT_COOP_HURT,          // CoopHurtEvent         -- co-op: "your character got hit here", to its owner
};

// Damage the partner's puppet took on this machine (an enemy shot it, mostly). The puppet itself
// takes none -- the amount is sent to the player it stands for, who takes it for real.
struct CoopHurtEvent {
    float amount;
};
static_assert(sizeof(CoopHurtEvent) == 4, "CoopHurtEvent layout");

// Where the host is: the story task (task-play-debug! takes graph node + task) and the host's
// position. Sent every 2 s while a partner is connected; the partner loads the task if it is
// not in it (also straight from the main menu) and then teleports next to the host.
struct CoopWhereEvent {
    NetSid64 node;           // task's parent node, e.g. SID("find-nora")
    NetSid64 task;           // task name,          e.g. SID("nor-scar-forest-combat")
    float    pos[3];
    float    rot[4];
};
static_assert(sizeof(CoopWhereEvent) == 44, "CoopWhereEvent layout");

// Co-op enemy sync. Both machines run the same chapter, so level enemies carry the same
// entity name (spawner hash) on both. Each side reports its enemies' health drops and deaths;
// the receiver applies a lower health and turns a death into a real kill of the same enemy.
struct CoopEnemyEntry {
    NetSid64 npc;            // entity name hash, e.g. SID("npc-glt-combat-forest-5")
    float    health;
    uint8_t  dead;
};
static_assert(sizeof(CoopEnemyEntry) == 13, "CoopEnemyEntry layout");
constexpr int kCoopEnemyPerEvent = 15;      // 15 * 13 = 195 B, under the 200 B event payload cap

// Host-authoritative enemy positions (unreliable, newest wins, ~15 Hz). The partner turns each
// listed enemy into a follower: its own AI is switched off and it moves to these positions.
struct CoopEnemyState {
    NetSid64 npc;            // entity name hash
    float    pos[3];
    float    yaw;            // radians around +Y
    uint8_t  flags;          // bit0 = dead on the host
};
static_assert(sizeof(CoopEnemyState) == 25, "CoopEnemyState layout");
constexpr int kMaxCoopEnemyStates = 48;     // 2 + 48 * 25 = 1202 B
struct CoopEnemiesPayload {
    uint16_t       count;
    CoopEnemyState enemies[kMaxCoopEnemyStates];
};

// Every enemy alive on the host, sent with NEVT_COOP_WHERE every 2 s. A partner that joined
// late (or walked into a trigger the host has not) reconciles against it: enemies the host
// does not have are removed, enemies only the host has are spawned from the same-named spawner.
struct CoopAliveEntry {
    NetSid64 npc;
    float    pos[3];
};
static_assert(sizeof(CoopAliveEntry) == 20, "CoopAliveEntry layout");
constexpr int kMaxCoopAlive = 190;          // 2 + 190 * 20 = 3802 B (session buffers hold 4 KB)
struct CoopAlivePayload {
    uint16_t       count;
    CoopAliveEntry enemies[kMaxCoopAlive];
};

struct ExecutionEvent {
    uint8_t  victimSlot;
    NetSid64 moveSID;
};
static_assert(sizeof(ExecutionEvent) == 9, "ExecutionEvent layout");

// A finisher is agreed BEFORE either machine commits to it. The attacker asks, the victim
// answers, and only an accepted request releases a puppet or starts a move. Without this both
// players could commit simultaneously and each end up holding a corpse for someone still alive
// -- reconcilable only after the fact, and only if both messages arrived.
struct ExecutionRequestEvent {
    uint8_t victimSlot;
};
static_assert(sizeof(ExecutionRequestEvent) == 1, "ExecutionRequestEvent layout");

struct ExecutionReplyEvent {
    uint8_t attackerSlot;
    uint8_t accepted;   // 0 = refused
};
static_assert(sizeof(ExecutionReplyEvent) == 2, "ExecutionReplyEvent layout");

struct AimMarkEvent {
    uint8_t markedSlot;
};
static_assert(sizeof(AimMarkEvent) == 1, "AimMarkEvent layout");

struct VoxLineEvent {
    uint8_t  speakerSlot;
    NetSid64 lineSID;
};
static_assert(sizeof(VoxLineEvent) == 9, "VoxLineEvent layout");

struct ProjectileThrowEvent {
    float    fromPos[3];
    float    toPos[3];
    float    vel[3];
    NetSid64 throwableSID;
};
static_assert(sizeof(ProjectileThrowEvent) == 44, "ProjectileThrowEvent layout");


struct MatchStateSync {
    uint8_t  timerRunning;
    float    timerRemaining;
    float    timerDuration;
    uint8_t  teamLives[2];
};
static_assert(sizeof(MatchStateSync) == 11, "MatchStateSync layout");

struct RespawnGrantEvent {
    NetSid64 spawnerSid; // which to1-spawn-point-N the host chose
    uint8_t  slot;
    uint8_t  team;
};
static_assert(sizeof(RespawnGrantEvent) == 10, "RespawnGrantEvent layout");

struct PlayerDamageEvent {
    NetSid64 weaponSID;
    NetSid64 weaponAmmoSID;
    float    damage;
    uint8_t  lethalClaim;
    uint8_t  headshot;
};
static_assert(sizeof(PlayerDamageEvent) == 22, "PlayerDamageEvent layout");

struct PlayerDeathEvent {
    NetSid64 weaponSID;
    uint8_t  headshotKill;
    uint8_t  victimSlot;
    uint8_t  killerSlot;
};
static_assert(sizeof(PlayerDeathEvent) == 11, "PlayerDeathEvent layout");

struct DoorOpenEvent {
    NetSid64 doorSid;
    NetSid64 stateSid;
};
static_assert(sizeof(DoorOpenEvent) == 16, "DoorOpenEvent layout");

struct FireShotEvent {
    float ori[4];
    float org[4];
    float launchVel[3];
};
static_assert(sizeof(FireShotEvent) == 44, "FireShotEvent layout");

struct MatchOverEvent {
    int8_t winner;
};
static_assert(sizeof(MatchOverEvent) == 1, "MatchOverEvent layout");

struct PingEvent {
    uint32_t sendMs;
    uint16_t hostPingMs;
    uint8_t  reply;
};
static_assert(sizeof(PingEvent) == 7, "PingEvent layout");

struct DebuggingEvent {
    uint16_t flags;
};
static_assert(sizeof(DebuggingEvent) == 2, "DebuggingEvent layout");

constexpr int kNetProcessNameLen = 64;

struct SuspiciousProcessEvent {
    uint32_t pid;
    char     processName[kNetProcessNameLen];
};
static_assert(sizeof(SuspiciousProcessEvent) == 68, "SuspiciousProcessEvent layout");

#pragma pack(pop)
