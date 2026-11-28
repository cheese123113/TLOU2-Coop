#include "coop/coop.hpp"
#include "coop/enemysync.hpp"
#include "coop/worldsync.hpp"

#include "engine/script.hpp"
#include "engine/structs.hpp"
#include "engine/draw.hpp"
#include "net-session.hpp"
#include "net-steam.hpp"
#include "net-engine.hpp"
#include "net-hooks.hpp"
#include "net-player.hpp"
#include "log.hpp"
#include "sid/sidbase.hpp"
#include "loader/config.hpp"
#include "net-common.hpp"   // g_base
#include "engine/gameinfo.hpp"

#include <safetyhook.hpp>

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <intrin.h>   // _ReturnAddress

extern bool multiplayer;   // match.cpp: gates the session/replication half of NetTick

namespace coop {

    namespace {

        // ---------------------------------------------------------------- config
        struct Config {
            std::string name = "Player";
            std::string look = "auto";          // "auto" = the look of the local player's own spawner
            std::string joinIp;                 // empty = F8 joins a Steam friend who is hosting
            std::vector<std::string> spawners;  // optional override: puppet spawner names to try first
            bool overlay = true;
            bool nametags = true;               // partner name above their character
            bool friendlyFire = false;          // players' bullets hurt each other (the host's setting counts)
            std::string bootNode, bootTask;     // test only: boot_task=<node>/<task>, loaded from the main menu
            uint32_t nrSeed = 0;                // test only: No Return seed to force (nr_seed=<hex>), 0 = off
        };
        Config g_cfg;
        std::atomic<bool> g_hostFriendlyFire{ false };   // guest: the host's setting (NEVT_COOP_WHERE)

        std::string Trim(std::string s)
        {
            const char* ws = " \t\r\n";
            s.erase(0, s.find_first_not_of(ws));
            const size_t e = s.find_last_not_of(ws);
            if (e == std::string::npos) return {};
            s.erase(e + 1);
            return s;
        }

        std::string GameDir()
        {
            char path[MAX_PATH];
            const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
            if (n == 0 || n >= MAX_PATH) return {};
            std::string s(path, n);
            const size_t slash = s.find_last_of("\\/");
            return slash == std::string::npos ? std::string{} : s.substr(0, slash + 1);
        }

        void WriteDefaultConfig(const std::string& path)
        {
            std::ofstream f(path);
            if (!f) return;
            f << "; TLOU2 campaign co-op settings. Lines starting with ; are comments.\n"
                 "; name    = shown to your partner\n"
                 "; look    = how your partner sees you. auto = your current campaign character\n"
                 "; join_ip = leave empty to join Steam friends with F8. Only set an IP for LAN play.\n"
                 "; spawner = optional, comma separated NPC spawner names to use for the partner puppet\n"
                 "; overlay = 1 shows the co-op status line in the top-left corner\n"
                 "; nametags = 1 shows your partner's name above their character\n"
                 "; friendlyfire = 1 lets your shots hurt your partner (the host's setting counts for both)\n"
                 "name=Player\n"
                 "look=auto\n"
                 "join_ip=\n"
                 "spawner=\n"
                 "overlay=1\n"
                 "nametags=1\n"
                 "friendlyfire=0\n";
        }

        void LoadConfig()
        {
            const std::string path = GameDir() + "tlou2-coop.ini";
            std::ifstream f(path);
            if (!f) {
                WriteDefaultConfig(path);
                TO1_LOG("[coop] no tlou2-coop.ini -- wrote defaults to %s\n", path.c_str());
                return;
            }
            std::string line;
            while (std::getline(f, line)) {
                line = Trim(line);
                if (line.empty() || line[0] == ';' || line[0] == '#') continue;
                const size_t eq = line.find('=');
                if (eq == std::string::npos) continue;
                const std::string key = Trim(line.substr(0, eq));
                const std::string val = Trim(line.substr(eq + 1));
                if (key == "name" && !val.empty())        g_cfg.name = val.substr(0, 23);
                else if (key == "look" && !val.empty())   g_cfg.look = val.substr(0, 39);
                else if (key == "join_ip")                g_cfg.joinIp = val;
                else if (key == "overlay")                g_cfg.overlay = (val != "0");
                else if (key == "nametags")               g_cfg.nametags = (val != "0");
                else if (key == "friendlyfire")           g_cfg.friendlyFire = (val == "1");
                else if (key == "boot_task") {
                    const size_t slash = val.find('/');
                    if (slash != std::string::npos) { g_cfg.bootNode = Trim(val.substr(0, slash)); g_cfg.bootTask = Trim(val.substr(slash + 1)); }
                }
                else if (key == "nr_seed")                g_cfg.nrSeed = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 16));
                else if (key == "spawner") {
                    size_t start = 0;
                    while (start <= val.size()) {
                        const size_t comma = val.find(',', start);
                        const std::string one = Trim(val.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
                        if (!one.empty()) g_cfg.spawners.push_back(one);
                        if (comma == std::string::npos) break;
                        start = comma + 1;
                    }
                }
            }
            TO1_LOG("[coop] config: name=\"%s\" look=\"%s\" join_ip=%s spawners=%zu overlay=%d\n",
                g_cfg.name.c_str(), g_cfg.look.c_str(), g_cfg.joinIp.c_str(), g_cfg.spawners.size(), g_cfg.overlay ? 1 : 0);
        }

        // ---------------------------------------------------------------- safe memory reads
        // Everything below walks engine memory we only partly understand. An access violation
        // must never happen at all: the game's crash reporter (crs-client) takes the fault
        // before our __except sees it and the process dies. So every address is checked with
        // VirtualQuery first (committed, readable, not a guard page) -- the __try stays only as
        // a second line for pages freed between the check and the read.
        bool Readable(uintptr_t addr, size_t n, bool write = false)
        {
            if (addr < 0x10000 || addr > 0x7FFFFFFFFFFFull || n == 0) return false;
            static uintptr_t s_lo = 0, s_hi = 0;      // last region known good for reading
            static bool s_loWritable = false;
            const uintptr_t end = addr + n;
            if (addr >= s_lo && end <= s_hi && (!write || s_loWritable)) return true;

            uintptr_t cur = addr;
            while (cur < end) {
                MEMORY_BASIC_INFORMATION mbi{};
                if (!VirtualQuery(reinterpret_cast<const void*>(cur), &mbi, sizeof(mbi))) return false;
                if (mbi.State != MEM_COMMIT) return false;
                const DWORD p = mbi.Protect;
                if (p & (PAGE_GUARD | PAGE_NOACCESS)) return false;
                const DWORD readMask = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
                const DWORD writeMask = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
                if (!(p & (write ? writeMask : readMask))) return false;
                const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                if (cur == addr) {
                    s_lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                    s_hi = regionEnd;
                    s_loWritable = (p & writeMask) != 0;
                }
                cur = regionEnd;
            }
            return true;
        }

        bool SafeRead(uintptr_t addr, void* out, size_t n)
        {
            if (!Readable(addr, n)) return false;
            __try { memcpy(out, reinterpret_cast<const void*>(addr), n); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        bool SafeWrite8(uintptr_t addr, uint8_t v)
        {
            if (!Readable(addr, 1, true)) return false;
            __try { *reinterpret_cast<volatile uint8_t*>(addr) = v; return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        bool UnitQuat(const float* q)
        {
            for (int i = 0; i < 4; ++i) if (!(q[i] == q[i]) || q[i] < -1.01f || q[i] > 1.01f) return false;
            const float len = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
            return len > 0.98f && len < 1.02f;
        }

        bool PlausiblePtr(uintptr_t p) { return p >= 0x10000 && p <= 0x7FFFFFFFFFFFull && (p & 7) == 0; }

        // Reads a NUL-terminated asset-style name ("npc-dina-03", "ellie_backpack"): 3..95 chars of
        // [a-z0-9-_.]. Anything else is treated as "not a name".
        bool ReadName(uintptr_t addr, std::string& out)
        {
            char buf[96];
            if (!SafeRead(addr, buf, sizeof(buf))) {
                // the string may sit near the end of a page: retry short
                if (!SafeRead(addr, buf, 32)) return false;
                buf[31] = 0;
            }
            size_t n = 0;
            for (; n < sizeof(buf); ++n) {
                const char c = buf[n];
                if (c == 0) break;
                const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
                if (!ok) return false;
            }
            if (n < 3 || n >= sizeof(buf)) return false;
            out.assign(buf, n);
            return true;
        }

        // A pointer is a spawner when its name field (+0x50) reads as a name AND the engine's own
        // get-spawner lookup of that name hands back the very same pointer.
        bool IsSpawner(uintptr_t sp, std::string* nameOut = nullptr)
        {
            if (!PlausiblePtr(sp)) return false;
            // Layout checks first, so the engine lookup below only ever sees real names:
            // a unit rotation at +0x10, a process type, a level pointer, and a name at +0x50.
            uint8_t head[sizeof(Spawner)];
            if (!SafeRead(sp, head, sizeof(head))) return false;
            const Spawner& s = *reinterpret_cast<const Spawner*>(head);
            if (!UnitQuat(&s.m_rot.x)) return false;
            if (s.m_processType == 0) return false;
            if (!PlausiblePtr(reinterpret_cast<uintptr_t>(s.m_pLevel))) return false;
            const uintptr_t namePtr = reinterpret_cast<uintptr_t>(s.m_spawnerName);
            std::string name;
            if (!PlausiblePtr(namePtr & ~7ull) || !ReadName(namePtr, name)) return false;
            if (reinterpret_cast<uintptr_t>(ENTITY::GET_SPAWNER(name.c_str())) != sp) return false;
            if (nameOut) *nameOut = name;
            return true;
        }

        std::string SpawnerArtGroup(uintptr_t sp)
        {
            uintptr_t p = 0; std::string s;
            if (SafeRead(sp + offsetof(Spawner, m_artGroup), &p, sizeof(p)) && ReadName(p, s)) return s;
            return {};
        }

        // The look-name buffer TW's model-changer writes into (spawner + 0x188). Only meaningful
        // for player/NPC spawners that carry a look override.
        std::string SpawnerLook(uintptr_t sp)
        {
            std::string s;
            if (ReadName(sp + 0x188, s)) return s;
            return {};
        }

        StringId64 SpawnerProcessType(uintptr_t sp)
        {
            StringId64 t = 0;
            SafeRead(sp + offsetof(Spawner, m_processType), &t, sizeof(t));
            return t;
        }

        const char* GuessProcessTypeName(StringId64 t)
        {
            static const char* kNames[] = {
                "npc", "player", "buddy", "buddy-npc", "npc-buddy", "npc-human", "npc-infected", "human-npc",
                "infected-npc", "simple-npc", "npc-simple", "character", "process-character", "actor",
                "process-npc", "npc-process", "horse", "dog", "npc-dog", "simple-entity", "process-simple",
            };
            for (const char* n : kNames)
                if (ToStringId64(n) == t) return n;
            return "?";
        }

        // ---- Spawner tables (read from the exe, build 4188655) --------------------------------
        // get-spawner -> FUN_141c45f00(sid): a 32-entry cache, else engine component #6 (the level
        // manager) -> FUN_141a52200, which walks the manager's level array ([mgr+0x1E0], u32 count at
        // [mgr+0x1D8]) and asks each level FUN_141e9f780 -> FUN_14207a310. Per level, the level data
        // [level+0x100] holds every spawner of the level: Spawner** at +0x08, int count at +0x10
        // (plus a sorted {sid, spawner} index at +0x138 / +0x140). A spawner's name hash is at
        // +0x38 (that is what spawn-entity-at-with-look looks up), a second hash at +0x48.
        constexpr uintptr_t kMgrLvlArr = 0x1E0, kMgrLvlCount = 0x1D8;   // component #6
        constexpr uintptr_t kMgrLevels = 0x310, kMgrCount = 0x918;      // the FUN_1417ac5d0 view (fallback)
        constexpr uint32_t  kMaxLevels = 0x200;
        constexpr uintptr_t kLvlSpawnData = 0x100, kSdArray = 0x08, kSdCount = 0x10;
        constexpr uintptr_t kSpNameSid = 0x38, kSpSid2 = 0x48;

        uintptr_t ReadPtr(uintptr_t a) { uintptr_t v = 0; return (SafeRead(a, &v, 8) && PlausiblePtr(v)) ? v : 0; }

        // ---- level manager capture (fallback) -------------------------------------------------
        // FUN_1417ac5d0(levelMgr, ...) is called for every level-placed process the game spawns;
        // remembering its rcx gives the manager even if the component table read fails.
        constexpr uint32_t kRvaLookupAllLevels = 0x17AC5D0;
        SafetyHookInline g_hkLookupAllLevels;
        volatile uintptr_t g_capturedMgr = 0;

        uint8_t __fastcall hk_LookupAllLevels(uintptr_t mgr, uint64_t id, uintptr_t* outLevel, uintptr_t* outA, uintptr_t* outEnt)
        {
            if (mgr) g_capturedMgr = mgr;
            return g_hkLookupAllLevels.unsafe_fastcall<uint8_t>(mgr, id, outLevel, outA, outEnt);
        }

        void InstallLevelMgrCapture()
        {
            g_hkLookupAllLevels = safetyhook::create_inline(reinterpret_cast<void*>(g_base + kRvaLookupAllLevels),
                reinterpret_cast<void*>(hk_LookupAllLevels));
            TO1_LOG("[coop] level-manager capture hook %s\n", g_hkLookupAllLevels ? "installed" : "FAILED");
        }

        void ReadLevelArray(uintptr_t arr, uint32_t n, std::vector<uintptr_t>& out)
        {
            for (uint32_t i = 0; i < n && i < kMaxLevels; ++i)
                if (const uintptr_t l = ReadPtr(arr + i * 8)) out.push_back(l);
        }

        // Every loaded level, preferring the exact list get-spawner itself walks.
        void LoadedLevels(std::vector<uintptr_t>& out, bool verbose = false)
        {
            const uintptr_t comp6 = reinterpret_cast<uintptr_t>(engine::GetEngineComponent(6));
            if (PlausiblePtr(comp6)) {
                uint32_t n = 0;
                const uintptr_t arr = ReadPtr(comp6 + kMgrLvlArr);
                if (arr && SafeRead(comp6 + kMgrLvlCount, &n, 4) && n > 0 && n <= kMaxLevels) {
                    ReadLevelArray(arr, n, out);
                    if (verbose) TO1_LOG("[coop] level manager (component 6) %p: %u levels\n", reinterpret_cast<void*>(comp6), n);
                    return;
                }
                if (verbose) TO1_LOG("[coop] component 6 %p: no level array (arr=%p n=%u)\n",
                    reinterpret_cast<void*>(comp6), reinterpret_cast<void*>(arr), n);
            }
            if (const uintptr_t mgr = g_capturedMgr) {
                uint32_t n = 0;
                if (SafeRead(mgr + kMgrCount, &n, 4) && n > 0 && n <= (kMgrCount - kMgrLevels) / 8) {
                    ReadLevelArray(mgr + kMgrLevels, n, out);
                    if (verbose) TO1_LOG("[coop] level manager (captured) %p: %u levels\n", reinterpret_cast<void*>(mgr), n);
                }
            }
        }

        // All spawners of one level.
        void LevelSpawners(uintptr_t level, std::vector<uintptr_t>& out)
        {
            const uintptr_t sd = ReadPtr(level + kLvlSpawnData);
            if (!sd) return;
            const uintptr_t arr = ReadPtr(sd + kSdArray);
            int32_t n = 0;
            if (!arr || !SafeRead(sd + kSdCount, &n, 4) || n <= 0 || n > 100000) return;
            for (int32_t i = 0; i < n; ++i)
                if (const uintptr_t sp = ReadPtr(arr + static_cast<uintptr_t>(i) * 8)) out.push_back(sp);
        }

        StringId64 SpawnerSid(uintptr_t sp) { StringId64 v = 0; SafeRead(sp + kSpNameSid, &v, 8); return v; }

        // The readable name, if the spawner keeps one (Twelve Weeks' layout puts a char* at +0x50).
        std::string SpawnerName(uintptr_t sp)
        {
            uintptr_t p = 0; std::string s;
            if (SafeRead(sp + offsetof(Spawner, m_spawnerName), &p, 8) && p >= 0x10000 && p <= 0x7FFFFFFFFFFFull && ReadName(p, s)) return s;
            return {};
        }

        struct SpawnerInfo { uintptr_t sp; StringId64 sid; std::string name; StringId64 type; std::string art; };

        void CollectSpawners(std::vector<SpawnerInfo>& out, bool verbose)
        {
            std::vector<uintptr_t> levels;
            LoadedLevels(levels, verbose);
            for (uintptr_t lvl : levels) {
                std::vector<uintptr_t> sps;
                LevelSpawners(lvl, sps);
                for (uintptr_t sp : sps)
                    out.push_back({ sp, SpawnerSid(sp), SpawnerName(sp), SpawnerProcessType(sp), SpawnerArtGroup(sp) });
            }
            if (verbose) TO1_LOG("[coop] %zu spawners in %zu levels\n", out.size(), levels.size());
        }

        // A character spawner: its art group reads like a character / NPC look.
        bool LooksLikeCharacter(const SpawnerInfo& s)
        {
            static const char* kHints[] = {
                "npc", "male", "female", "wlf", "militia", "scar", "seraph", "infected", "runner", "clicker",
                "stalker", "shambler", "dina", "jesse", "tommy", "joel", "lev", "yara", "manny", "mel", "owen",
                "jordan", "nora", "isaac", "dog", "rattler", "ellie", "abby", "human",
            };
            const std::string& a = s.art.empty() ? s.name : s.art;
            if (a.empty()) return false;
            for (const char* h : kHints) if (a.find(h) != std::string::npos) return true;
            return false;
        }

        uintptr_t g_playerSpawner = 0;
        std::string g_playerLook;

        // The local player's spawner: a spawner whose name or art group says "player".
        uintptr_t PlayerSpawner()
        {
            std::vector<SpawnerInfo> all;
            CollectSpawners(all, false);
            for (const SpawnerInfo& s : all)
                if (s.sid == SID("player") || s.name == "player") return s.sp;
            return 0;
        }

        void HexDump(const char* tag, uintptr_t addr, size_t n)
        {
            uint8_t buf[0x100];
            if (n > sizeof(buf)) n = sizeof(buf);
            if (!SafeRead(addr, buf, n)) { TO1_LOG("[coop]   %s %p: unreadable\n", tag, reinterpret_cast<void*>(addr)); return; }
            for (size_t row = 0; row < n; row += 16) {
                char line[128]; int k = 0;
                for (size_t i = row; i < row + 16 && i < n; i += 8)
                    k += snprintf(line + k, sizeof(line) - k, " %016llX", *reinterpret_cast<unsigned long long*>(buf + i));
                TO1_LOG("[coop]   %s +%02zX:%s\n", tag, row, line);
            }
        }

        // ---- current story task (for "load where the host is") ------------------------------
        // task-get-current (no args) returns the current task object; task-play-debug! takes
        // (a, b, c) and plays TaskMgr.Find(a, b) -- a graph node and a task inside it. This probe
        // dumps the task object and names every string id in it (and one pointer deeper), so
        // the offsets of a and b can be read from the log. Names come from a sidbase.bin if one
        // is on disk (Twelve Weeks leaves one in tlou-ii-mp\); without it only hashes show.
        const char* SidName(StringId64 sid)
        {
            static bool tried = false;
            if (!tried) {
                tried = true;
                if (!NdGameSdk::Sidbase::InitSidbase(PROJECT_DATA_DIR "/sidbase.bin"))
                    NdGameSdk::Sidbase::InitSidbase("tlou-ii-mp/sidbase.bin");
            }
            const char* s = NdGameSdk::Sidbase::StringIdToStringInternal(sid);
            return (s && s[0] != '#') ? s : nullptr;
        }

        void ProbeTask()
        {
            const uintptr_t task = static_cast<uintptr_t>(DC::Call<"task-get-current">());
            TO1_LOG("[coop] current task object: %p\n", reinterpret_cast<void*>(task));
            if (!PlausiblePtr(task)) return;
            HexDump("task", task, 0x100);
            for (uintptr_t off = 0; off < 0x200; off += 8) {
                uint64_t q = 0;
                if (!SafeRead(task + off, &q, 8)) continue;
                if (const char* n = SidName(q))
                    TO1_LOG("[coop]   task+0x%03llX = %016llX \"%s\"\n", static_cast<unsigned long long>(off), static_cast<unsigned long long>(q), n);
                else if (PlausiblePtr(q)) {
                    for (uintptr_t o2 = 0; o2 < 0x80; o2 += 8) {
                        uint64_t q2 = 0;
                        if (!SafeRead(q + o2, &q2, 8)) continue;
                        if (const char* n2 = SidName(q2))
                            TO1_LOG("[coop]   task+0x%03llX -> +0x%02llX = %016llX \"%s\"\n", static_cast<unsigned long long>(off),
                                static_cast<unsigned long long>(o2), static_cast<unsigned long long>(q2), n2);
                    }
                }
            }
        }

        void ProbeAroundPlayer()
        {
            TO1_LOG("[coop] ===== discovery probe =====\n");
            ProbeTask();
            std::vector<SpawnerInfo> all;
            CollectSpawners(all, true);
            int dumped = 0, chars = 0, shown = 0;
            for (const SpawnerInfo& s : all) {
                const bool ch = LooksLikeCharacter(s);
                chars += ch ? 1 : 0;
                if (dumped < 2) { HexDump(dumped == 0 ? "spawner[0]" : "spawner[1]", s.sp, 0xC0); ++dumped; }
                if (shown < 400 && (ch || shown < 60)) {
                    ++shown;
                    TO1_LOG("[coop]   %s sid=%016llX name=\"%s\" type=%016llX(%s) art=\"%s\" look=\"%s\"\n",
                        ch ? "CHAR" : "    ", static_cast<unsigned long long>(s.sid), s.name.c_str(),
                        static_cast<unsigned long long>(s.type), GuessProcessTypeName(s.type), s.art.c_str(), SpawnerLook(s.sp).c_str());
                }
            }
            TO1_LOG("[coop] ===== probe done: %zu spawners, %d look like characters =====\n", all.size(), chars);
        }

        // The look the local player is wearing: the player process points at its look name
        // somewhere in its first pages. Any pointer there that reads as an "ellie..." / "abby..."
        // (or other playable) name AND exists in the look2/characters module is taken.
        std::string PlayerLookFromProcess()
        {
            const uintptr_t proc = static_cast<uintptr_t>(DcManager::LookupObject(SID("player")));
            if (!PlausiblePtr(proc)) return {};
            static const char* kPrefixes[] = { "ellie", "abby", "lev", "dina", "joel", "yara" };
            for (uintptr_t off = 0; off < 0x4000; off += 8) {
                uintptr_t p = 0;
                if (!SafeRead(proc + off, &p, 8) || p < 0x10000 || p > 0x7FFFFFFFFFFFull) continue;
                std::string n;
                if (!ReadName(p, n)) continue;
                bool pref = false;
                for (const char* k : kPrefixes) pref |= (n.rfind(k, 0) == 0);
                if (!pref) continue;
                if (!DcManager::LookupInModule(ToStringId64(n.c_str()), SID("look2/characters"))) continue;
                TO1_LOG("[coop] player look from process+0x%llX: \"%s\"\n", static_cast<unsigned long long>(off), n.c_str());
                return n;
            }
            TO1_LOG("[coop] no look name found in the player process\n");
            return {};
        }

        // NPC spawners: level designers name them "npc-..." and they share one process type
        // (0x99ABC3A158822080 in the Seattle levels). The type is learned from the "npc-" names
        // so other chapters work too. Human NPCs come first (closest skeleton to the player).
        bool IsInfectedArt(const std::string& a)
        {
            static const char* kInf[] = { "infected", "clicker", "runner", "stalker", "shambler", "bloater", "rattler", "dog" };
            for (const char* k : kInf) if (a.find(k) != std::string::npos) return true;
            return false;
        }

        void PuppetSpawnerCandidates(std::vector<SpawnerInfo>& out)
        {
            for (const std::string& n : g_cfg.spawners) {
                const uintptr_t sp = reinterpret_cast<uintptr_t>(ENTITY::GET_SPAWNER(n.c_str()));
                if (sp) out.push_back({ sp, SpawnerSid(sp), n, SpawnerProcessType(sp), SpawnerArtGroup(sp) });
                else TO1_LOG("[coop] ini spawner \"%s\" is not in this level\n", n.c_str());
            }
            // Only HUMAN NPC spawners, ever. A dog or infected spawner given a human look puts a
            // human skeleton's joint indices on a dog body and crashes the engine's joint code
            // (seen with "npc-res-dog-on-leash-*", the first "npc-" spawner in Hillcrest).
            // The human NPC type is the most common process type among "npc-" spawners whose art
            // group names a person; spawners with an empty art group are skipped (no way to tell).
            std::vector<SpawnerInfo> all;
            CollectSpawners(all, false);
            auto isPersonArt = [](const std::string& a) {
                if (a.empty() || IsInfectedArt(a)) return false;
                static const char* kPerson[] = { "militia", "scar", "wlf", "seraph", "male", "female", "human" };
                for (const char* k : kPerson) if (a.find(k) != std::string::npos) return true;
                return false;
            };
            std::vector<std::pair<StringId64, int>> votes;
            for (const SpawnerInfo& s : all) {
                if (!s.type || s.name.rfind("npc-", 0) != 0 || !isPersonArt(s.art)) continue;
                bool found = false;
                for (auto& v : votes) if (v.first == s.type) { ++v.second; found = true; }
                if (!found) votes.push_back({ s.type, 1 });
            }
            StringId64 humanType = 0; int best = 0;
            for (auto& v : votes) if (v.second > best) { best = v.second; humanType = v.first; }
            if (!humanType) {
                // Fallback (Seattle's open-world hub loads only infected NPC spawners): borrow a
                // humanoid INFECTED spawner. Infected are humans underneath, so a human look fits
                // their skeleton; dogs and the big / deformed ones (clicker, bloater, shambler,
                // rat king, stalker) have other skeletons and are never used. The puppet's AI is
                // switched off right after the spawn either way.
                std::vector<std::pair<StringId64, int>> infVotes;
                for (const SpawnerInfo& s : all) {
                    if (!s.type || s.name.rfind("npc-", 0) != 0) continue;
                    if (s.name.find("inf") == std::string::npos && !IsInfectedArt(s.art)) continue;
                    bool found = false;
                    for (auto& v : infVotes) if (v.first == s.type) { ++v.second; found = true; }
                    if (!found) infVotes.push_back({ s.type, 1 });
                }
                StringId64 infType = 0; int infBest = 0;
                for (auto& v : infVotes) if (v.second > infBest) { infBest = v.second; infType = v.first; }
                // (not "horse": the Sea Horse Hotel names all of Seattle's hub infected "npc-sea-horse-hotel-...")
                static const char* kNotHumanoid[] = { "dog", "clicker", "bloater", "shambler", "rat-king", "ratking", "stalker" };
                for (const SpawnerInfo& s : all) {
                    if (!s.sid || !infType || s.type != infType || s.name.rfind("npc-", 0) != 0) continue;
                    bool bad = false;
                    for (const char* k : kNotHumanoid) bad |= (s.name.find(k) != std::string::npos || s.art.find(k) != std::string::npos);
                    if (!bad) out.push_back(s);
                }
                TO1_LOG("[coop] no human NPC spawner here -- fallback: %zu humanoid infected spawner candidates (type %016llX)\n",
                    out.size(), static_cast<unsigned long long>(infType));
                return;
            }

            for (const SpawnerInfo& s : all)
                if (s.sid && s.type == humanType && s.name.rfind("npc-", 0) == 0 && isPersonArt(s.art) &&
                    !IsInfectedArt(s.name) && s.name.find("dog") == std::string::npos)
                    out.push_back(s);
            TO1_LOG("[coop] %zu human NPC spawner candidates (type %016llX)\n", out.size(), static_cast<unsigned long long>(humanType));
        }

        // ---------------------------------------------------------------- guest save block
        // A joined partner plays the HOST's chapter, so any save it writes would overwrite its own
        // campaign progress. Every save request (checkpoints, manual saves, settings) runs through
        // exe 0x141e38290(req): it starts the writer on [req+0x108], waits for it, and returns the
        // writer's status ([[req+0x108]+0x14], 0 = OK, negative = SCE error). While blocked, the
        // writer is never started: the status is set to 0 and 0 returned, so the game carries on
        // as after a normal save. Runs on the save thread, hence the atomics.
        constexpr uint32_t kRvaSaveRequest = 0x1E38290;
        SafetyHookInline g_hkSaveRequest;
        std::atomic<bool> g_blockSaves{ false };
        std::atomic<int>  g_savesBlocked{ 0 };
        bool g_testBlockSaves = false;                     // Shift+F10, for solo testing

        uint32_t __fastcall hk_SaveRequest(uintptr_t req)
        {
            if (g_blockSaves.load(std::memory_order_relaxed)) {
                int slot = -1;
                SafeRead(req + 0x14, &slot, 4);
                const uintptr_t writer = ReadPtr(req + 0x108);
                if (writer) {
                    uint32_t ok = 0;
                    if (Readable(writer + 0x14, 4, true)) memcpy(reinterpret_cast<void*>(writer + 0x14), &ok, 4);
                }
                g_savesBlocked.fetch_add(1, std::memory_order_relaxed);
                TO1_LOG("[coop] save blocked (slot %d) -- saving is off (guest or test session), your own save is untouched\n", slot);
                return 0;
            }
            return g_hkSaveRequest.unsafe_fastcall<uint32_t>(req);
        }

        void InstallSaveBlock()
        {
            static const uint8_t kExpect[16] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x89, 0x08, 0x01, 0x00, 0x00 };
            const uintptr_t f = g_base + kRvaSaveRequest;
            if (!Readable(f, sizeof(kExpect)) || memcmp(reinterpret_cast<const void*>(f), kExpect, sizeof(kExpect)) != 0) {
                TO1_LOG("[coop] save block NOT installed: save function prologue differs (other game build?)\n");
                return;
            }
            g_hkSaveRequest = safetyhook::create_inline(reinterpret_cast<void*>(f), reinterpret_cast<void*>(hk_SaveRequest));
            TO1_LOG("[coop] save block hook %s\n", g_hkSaveRequest ? "installed" : "FAILED");
        }

        // ---------------------------------------------------------------- No Return seed (NR1c)
        // The run's random numbers come from one state object (exe 0x144162cd0, build 4188655):
        // the primary seed sits at +0x72CC (rogue-get-primary-seed reads it, unless a custom
        // run's override seed is active), and per-category generators (0x150-byte entries at
        // +0x72F0, count +0x7300) are seeded from it. Both setters below are what the
        // rogue-set-prng-seed / rogue-set-prng-category-seed natives call.
        //  - Every call is logged with the caller, to see when a new run picks its seed.
        //  - nr_seed=<hex> in tlou2-coop.ini (test only) replaces the seed the game sets: does
        //    the same seed give the same encounter board and factions? (co-op idea: the joined
        //    player's run uses the host's seed)
        constexpr uint32_t kRvaRogueSetSeed = 0x104BF60;           // (state*, u32 seed)
        constexpr uint32_t kRvaRogueSetCategorySeed = 0x104BE60;   // (state*, u64 category, u32 seed)
        SafetyHookInline g_hkRogueSetSeed, g_hkRogueSetCategorySeed;
        std::atomic<int> g_seedLogs{ 0 }, g_catLogs{ 0 };

        uint64_t __fastcall hk_RogueSetSeed(uintptr_t state, uint32_t seed)
        {
            const unsigned long long caller = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base;
            const uint32_t use = g_cfg.nrSeed ? g_cfg.nrSeed : seed;
            if (g_seedLogs.fetch_add(1) < 60) {
                if (use != seed) TO1_LOG("[nr] seed set %08X -> replaced by nr_seed %08X (from exe+%llX)\n", seed, use, caller);
                else TO1_LOG("[nr] seed set %08X (from exe+%llX)\n", seed, caller);
            }
            return g_hkRogueSetSeed.unsafe_fastcall<uint64_t>(state, use);
        }

        uint64_t __fastcall hk_RogueSetCategorySeed(uintptr_t state, uint64_t category, uint32_t seed)
        {
            if (g_catLogs.fetch_add(1) < 200)
                TO1_LOG("[nr] category seed %016llX = %08X (from exe+%llX)\n", static_cast<unsigned long long>(category), seed,
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base));
            return g_hkRogueSetCategorySeed.unsafe_fastcall<uint64_t>(state, category, seed);
        }

        void InstallRogueSeedHooks()
        {
            static const uint8_t kSeed[10] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20 };
            static const uint8_t kCat[15] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30 };
            const uintptr_t s = g_base + kRvaRogueSetSeed, c = g_base + kRvaRogueSetCategorySeed;
            if (Readable(s, sizeof(kSeed)) && memcmp(reinterpret_cast<const void*>(s), kSeed, sizeof(kSeed)) == 0)
                g_hkRogueSetSeed = safetyhook::create_inline(reinterpret_cast<void*>(s), reinterpret_cast<void*>(hk_RogueSetSeed));
            if (Readable(c, sizeof(kCat)) && memcmp(reinterpret_cast<const void*>(c), kCat, sizeof(kCat)) == 0)
                g_hkRogueSetCategorySeed = safetyhook::create_inline(reinterpret_cast<void*>(c), reinterpret_cast<void*>(hk_RogueSetCategorySeed));
            TO1_LOG("[nr] seed hooks: primary %s, category %s; nr_seed %s%08X\n",
                g_hkRogueSetSeed ? "installed" : "NOT installed", g_hkRogueSetCategorySeed ? "installed" : "NOT installed",
                g_cfg.nrSeed ? "= " : "off ", g_cfg.nrSeed);
        }

        // ---------------------------------------------------------------- session control
        enum class Want { None, Host, Join };

        float g_lastSpawnTry = -100.0f;
        int   g_spawnFails = 0;
        float g_clock = 0.0f;
        bool  g_hooksOn = false;
        int   g_guestOwnDifficulty = -1;               // our difficulty before matching the host's
        float g_nextBeacon = 0.0f;
        void Flash(const char* msg);
        void BeginSteamSearch();

        void EnsureLocalIdentity()
        {
            // "Player" (the default) -> the Steam name, so the partner sees who they joined.
            const bool useSteamName = (g_cfg.name == "Player") && net::steam::Available();
            net::session::SetLocalName(useSteamName ? net::steam::MyName() : g_cfg.name.c_str());

            std::string look = g_cfg.look;
            if (look == "auto") {
                // The current story day says whose day it is (see LocalCharacterLook).
                look = coop::LocalCharacterLook();
                TO1_LOG("[coop] auto look -> \"%s\"\n", look.c_str());
            }
            net::session::SetLocalLook(look.c_str());
        }

        void StartSession(Want w)
        {
            // Hosting needs a loaded game (the host decides where everyone plays). Joining works
            // from the main menu too: the host's NEVT_COOP_WHERE then loads its story task.
            if (w == Want::Host && !ENTITY::DOES_EXIST(SID("player"))) {
                TO1_LOG("[coop] host ignored: load into the campaign first\n");
                Flash("Load into the campaign first, then press F7 to host");
                return;
            }
            EnsureLocalIdentity();
            if (!g_hooksOn) { net::hooks::InstallCoopHooks(); g_hooksOn = true; }
            multiplayer = true;
            g_spawnFails = 0;
            coop::enemies::Reset();
            coop::world::Reset();
            g_lastSpawnTry = -100.0f;

            if (w == Want::Host) {
                TO1_LOG("[coop] F7: hosting (Steam friends + UDP 7777)\n");
                net::session::HostGame();
                g_nextBeacon = 0.0f;
                Flash(net::steam::Available() ? "Hosting - your Steam friend presses F8 to join"
                                              : "Hosting on UDP 7777 (Steam unavailable)");
            }
            else if (!g_cfg.joinIp.empty()) {
                TO1_LOG("[coop] F8: joining %s:7777\n", g_cfg.joinIp.c_str());
                net::session::JoinGame(g_cfg.joinIp.c_str());
                Flash("Joining by IP...");
            }
            else {
                BeginSteamSearch();
            }
        }

        // ---------------------------------------------------------------- "join where the host is"
        // The current story task, read off task-get-current's object (offsets from the task
        // probe, build 4188655): task+0x08 = task name, [task+0x120]+0x08 = parent node,
        // [task+0x120]+0x48 = the day's display name ("Ellie Day 2", "Abby Day 1", ...).
        // task-play-debug!(node, task, 0) loads exactly that task, from the menu or in game.
        struct TaskId { StringId64 node = 0, task = 0, day = 0; };

        bool CurrentTask(TaskId& out)
        {
            const uintptr_t t = static_cast<uintptr_t>(DC::Call<"task-get-current">());
            if (!PlausiblePtr(t)) return false;
            uint64_t task = 0;
            if (!SafeRead(t + 0x08, &task, 8) || !task) return false;
            const uintptr_t node = ReadPtr(t + 0x120);
            uint64_t nodeName = 0, day = 0;
            if (!node || !SafeRead(node + 0x08, &nodeName, 8) || !nodeName) return false;
            SafeRead(node + 0x48, &day, 8);
            out = { nodeName, task, day };
            return true;
        }

        void TeleportPlayer(const Vector3& p, const Quaternion& q)
        {
            DC::Call<"send-event">(SID("teleport-player"), SID("player"),
                MEMORY::NEW_BOXED_VALUE(BoxedValueType::kBoundFrame, MEMORY::ALLOC_BOUND_FRAME(p, q)));
        }

        // Host: every 2 s while a partner is connected.
        float g_nextWhere = 0.0f;

        // The host's "life": +1 whenever its player is recreated (death -> checkpoint restart,
        // a reload, a restart). Seen as a new player process, or as the player leaving the Death
        // state. The partner reloads with the host when it changes (OnWhere / TickWhereClient).
        uint32_t  g_hostLife = 0;
        uintptr_t g_hostProc = 0;
        bool      g_hostWasDead = false;

        // M2.8 counted the "Death" player state and every player-process change, and both flicker
        // in normal play (8 false "lives" in 80 s, each one reloading the partner). Only two
        // solid signals count now:
        //  - a real death: health 0 for at least 0.5 s, then alive again;
        //  - a real reload: a new player process AND the position jumped > 15 m at that moment.
        float   g_hostDeadSince = -1.0f;
        Vector3 g_hostLastPos{};

        void TrackHostLife()
        {
            const uintptr_t proc = static_cast<uintptr_t>(DcManager::LookupObject(SID("player")));
            if (!proc) return;
            const Vector3 pos = LOCATOR::GET_TRANSFORM(SID("player")).Position;
            const int hp = PLAYER::GET_HEALTH();

            const char* why = nullptr;
            if (hp <= 0) {
                if (g_hostDeadSince < 0.0f) g_hostDeadSince = g_clock;
                if (g_clock - g_hostDeadSince >= 0.5f) g_hostWasDead = true;
            }
            else {
                g_hostDeadSince = -1.0f;
                if (g_hostWasDead) { g_hostWasDead = false; why = "died and respawned"; }
            }
            if (g_hostProc && proc != g_hostProc) {
                const float dx = pos.x - g_hostLastPos.x, dy = pos.y - g_hostLastPos.y, dz = pos.z - g_hostLastPos.z;
                const float jump = std::sqrt(dx * dx + dy * dy + dz * dz);
                // (The lookup hands out a different process pointer many times a second in
                // normal play, so only a real jump is worth a log line.)
                if (jump > 15.0f) {
                    if (!why) why = "reloaded (new player, moved far)";
                    TO1_LOG("[coop] host player process changed and moved %.1f m\n", jump);
                }
            }
            g_hostProc = proc;
            g_hostLastPos = pos;

            static float s_lastBump = -100.0f;              // one count per event, even if both signals fire
            if (why && g_clock - s_lastBump > 5.0f) {
                s_lastBump = g_clock;
                ++g_hostLife;
                g_nextWhere = 0.0f;                          // tell the partner right away
                coop::world::OnHostWorldReset();             // taken pickups / opened doors since the checkpoint are back
                TO1_LOG("[coop] host %s -> life %u\n", why, g_hostLife);
            }
        }

        void TickWhereHost()
        {
            if (!net::session::IsHost()) return;
            TrackHostLife();
            if (!net::session::IsConnected()) return;
            if (g_clock < g_nextWhere) return;
            g_nextWhere = g_clock + 2.0f;
            TaskId id;
            if (!ENTITY::DOES_EXIST(SID("player")) || !CurrentTask(id)) return;
            const Transform t = LOCATOR::GET_TRANSFORM(SID("player"));
            CoopWhereEvent e{};
            e.node = id.node; e.task = id.task;
            e.pos[0] = t.Position.x; e.pos[1] = t.Position.y; e.pos[2] = t.Position.z;
            e.rot[0] = t.Rotation.x; e.rot[1] = t.Rotation.y; e.rot[2] = t.Rotation.z; e.rot[3] = t.Rotation.w;
            e.life = g_hostLife;
            const uint64_t diff = DC::Call<"get-difficulty">();
            e.difficulty = diff <= 0xFE ? static_cast<uint8_t>(diff) : 0xFF;
            e.flags = g_cfg.friendlyFire ? kCoopWhereFriendlyFire : 0;
            static uint64_t s_loggedDiff = ~0ull;
            if (diff != s_loggedDiff) { s_loggedDiff = diff; TO1_LOG("[coop] host difficulty = %llu\n", static_cast<unsigned long long>(diff)); }
            net::session::SendEvent(NEVT_COOP_WHERE, e);
            coop::enemies::SendAliveSnapshot();          // late-join reconcile on the partner
        }

        // A story task the mod loads from the main menu (boot_task, joining from the menu) leaves
        // the front-end menu stack up behind the game, so the menu keeps the mouse and the
        // camera cannot be moved. Two seconds after the player exists the mod closes it:
        // menu2-pop-page until the main stack is empty, then menu2-hide-page-stack. Only after
        // the mod's own loads -- never a menu the player opened.
        bool  g_closeMenuAfterLoad = false;
        float g_menuLoadPlayerSince = -1.0f;

        void PlayTaskFromMod(StringId64 node, StringId64 task)
        {
            if (!ENTITY::DOES_EXIST(SID("player"))) {
                g_closeMenuAfterLoad = true;
                g_menuLoadPlayerSince = -1.0f;
            }
            DEBUG::PLAY_TASK(node, task, 0);
        }

        void TickCloseMenuAfterLoad()
        {
            if (!g_closeMenuAfterLoad) return;
            if (!ENTITY::DOES_EXIST(SID("player"))) { g_menuLoadPlayerSince = -1.0f; return; }
            if (g_menuLoadPlayerSince < 0.0f) { g_menuLoadPlayerSince = g_clock; return; }
            if (g_clock - g_menuLoadPlayerSince < 2.0f) return;
            g_closeMenuAfterLoad = false;
            const StringId64 first = MENU2::GET_TOP_PAGE_IN_STACK(MenuStack::main);
            int pops = 0;
            for (; pops < 10 && MENU2::GET_TOP_PAGE_IN_STACK(MenuStack::main); ++pops)
                DC::Call<"menu2-pop-page">(static_cast<uint64_t>(MenuStack::main), 0ull);
            // hide the stack only if popping did not empty it (a hidden stack might also hide
            // menus the player opens later)
            if (MENU2::GET_TOP_PAGE_IN_STACK(MenuStack::main))
                DC::Call<"menu2-hide-page-stack">(static_cast<uint64_t>(MenuStack::main), 1ull);
            const StringId64 left = MENU2::GET_TOP_PAGE_IN_STACK(MenuStack::main);
            const uint64_t shown = DC::Call<"menu2-is-page-stack-shown?">(static_cast<uint64_t>(MenuStack::main));
            TO1_LOG("[coop] closed the main menu after our load: top page %016llX, %d pop(s), now %016llX, stack shown %llu\n",
                static_cast<unsigned long long>(first), pops, static_cast<unsigned long long>(left),
                static_cast<unsigned long long>(shown & 0xFF));
        }

        // Partner: load the host's chapter part when not in it, then stand next to the host.
        // Within the same part (node) a different task is NOT reloaded -- tasks change at every
        // checkpoint and the partner normally walks through them alongside the host.
        struct WhereState {
            bool  have = false;
            CoopWhereEvent w{};
            bool  loadIssued = false;
            float loadAt = 0.0f;
            bool  placed = false;
            float playerSince = -1.0f;
            uintptr_t playerProc = 0;                      // our player process (changes on death / reload)
            uint32_t  life = 0;                            // the host's life we last followed
            bool      lifeKnown = false;
            bool      forceReload = false;                 // the host died / reloaded: reload with it
            float     lostPlayerAt = -100.0f;              // our player vanished (a loading screen started)
            bool      hadPlayer = false;                   // we had a player since joining (not the main menu)
            StringId64 placedNode = 0;                     // the chapter part we were last placed in
            float     aheadSince = -1.0f;                  // we walked into the next part before the host
        } g_where;

        // How long the partner may stand in the next chapter part, after walking into it before
        // the host, waiting for the host to arrive (the host's own loading screen) before it is
        // taken back to the host's part.
        constexpr float kAheadWait = 25.0f;

        void TickWhereClient()
        {
            if (!g_where.have || net::session::IsHost() || !net::session::IsRunning()) return;
            const uintptr_t proc = static_cast<uintptr_t>(DcManager::LookupObject(SID("player")));
            const bool hasPlayer = proc != 0;
            if (proc != g_where.playerProc) {
                // Our player was recreated: a load finished, or WE died and the checkpoint
                // restarted. Either way, stand next to the host again once it has settled.
                if (g_where.placed && proc) {
                    TO1_LOG("[coop] our player was recreated (death / reload) -> placing next to the host again\n");
                    Flash("Respawned - moving you back to the host");
                }
                if (!proc && g_where.playerProc) g_where.lostPlayerAt = g_clock;
                if (proc) g_where.hadPlayer = true;
                g_where.playerProc = proc;
                g_where.placed = false;
                g_where.playerSince = proc ? g_clock : -1.0f;
            }

            TaskId cur;
            const bool haveTask = hasPlayer && CurrentTask(cur);
            const bool inPart = haveTask && cur.node == g_where.w.node;
            if (inPart) {
                g_where.loadIssued = false;                // arrived: a later part change is a new event
                if (g_where.aheadSince >= 0.0f) {
                    g_where.aheadSince = -1.0f;
                    TO1_LOG("[coop] the host reached our chapter part -> placing next to them\n");
                }
            }
            if (inPart && g_where.forceReload) {
                // The host died or reloaded a checkpoint: its world reset, so reload ours too
                // (the enemy reconcile then matches the host's enemies again).
                g_where.forceReload = false;
                g_where.loadIssued = true;
                g_where.loadAt = g_clock;
                g_where.placed = false;
                g_where.playerSince = g_clock;
                coop::enemies::Reset();
                coop::world::Reset();
                TO1_LOG("[coop] host life %u -> reloading the host's task with it\n", g_where.life);
                Flash("The host respawned - reloading with them...");
                PlayTaskFromMod(g_where.w.node, g_where.w.task);
                return;
            }
            if (!inPart) {
                g_where.forceReload = false;               // a full load is coming anyway
                g_where.placed = false;
                // No player while we had one: a loading screen is running (the game's own chapter
                // change, or a load we started). Never start a load on top of it; wait until it
                // is done. (On the main menu there never was a player: load right away.)
                if (!hasPlayer && g_where.hadPlayer && g_clock - g_where.lostPlayerAt < 90.0f) return;
                // We walked into the next chapter part before the host (the host is still in the
                // part we were placed in): wait here for the host -- usually only its loading
                // screen behind -- and go back to the host only if it does not come.
                if (haveTask && !g_where.loadIssued && g_where.placedNode
                    && cur.node != g_where.placedNode && g_where.w.node == g_where.placedNode) {
                    if (g_where.aheadSince < 0.0f) {
                        g_where.aheadSince = g_clock;
                        TO1_LOG("[coop] we reached the next chapter part %016llX before the host -> waiting up to %.0f s\n",
                            static_cast<unsigned long long>(cur.node), kAheadWait);
                        Flash("You reached the next chapter first - waiting for the host...");
                    }
                    if (g_clock - g_where.aheadSince < kAheadWait) return;
                    g_where.aheadSince = -1.0f;
                    TO1_LOG("[coop] the host did not follow into the next part -> back to the host\n");
                    Flash("The host is still behind - taking you back to them...");
                    g_where.loadIssued = true;
                    g_where.loadAt = g_clock;
                    PlayTaskFromMod(g_where.w.node, g_where.w.task);
                    return;
                }
                g_where.aheadSince = -1.0f;
                if (!g_where.loadIssued || g_clock - g_where.loadAt > 60.0f) {
                    g_where.loadIssued = true;
                    g_where.loadAt = g_clock;
                    TO1_LOG("[coop] loading the host's task %016llX / %016llX\n",
                        static_cast<unsigned long long>(g_where.w.node), static_cast<unsigned long long>(g_where.w.task));
                    Flash("Loading the host's game...");
                    PlayTaskFromMod(g_where.w.node, g_where.w.task);
                }
                return;
            }
            // In the host's part: once the level has settled, stand 1.2 m beside the host.
            if (!g_where.placed && g_clock - g_where.playerSince > 3.0f) {
                g_where.placed = true;
                g_where.placedNode = cur.node;
                const Vector3 p{ g_where.w.pos[0] + 1.2f, g_where.w.pos[1], g_where.w.pos[2] };
                const Quaternion q{ g_where.w.rot[0], g_where.w.rot[1], g_where.w.rot[2], g_where.w.rot[3] };
                TeleportPlayer(p, q);
                TO1_LOG("[coop] placed next to the host at %.1f %.1f %.1f\n", p.x, p.y, p.z);
                Flash("Joined - you are next to the host");
            }
        }

        // ---------------------------------------------------------------- Steam friend lobby
        // Channel 1 carries two one-byte messages. 'Q' = "is anyone hosting?" (joiner -> each
        // friend in this game), 'H' = "I am hosting" (host -> each friend in this game, every 2 s,
        // and as the answer to 'Q'). Sending to a friend also opens the Steam session with them,
        // so after this exchange the game packets on channel 0 flow both ways.
        bool  g_searching = false;
        float g_searchUntil = 0.0f, g_nextQuery = 0.0f;

        void SendToFriendsInGame(char what)
        {
            std::vector<uint64_t> friends;
            net::steam::FriendsInThisGame(friends);
            for (uint64_t f : friends) net::steam::Send(f, &what, 1, net::steam::kChannelLobby);
        }

        void BeginSteamSearch()
        {
            if (!net::steam::Available()) {
                Flash("Steam is not available - set join_ip in tlou2-coop.ini for LAN play");
                return;
            }
            std::vector<uint64_t> friends;
            net::steam::FriendsInThisGame(friends);
            TO1_LOG("[coop] F8: looking for a hosting friend among %zu friend(s) in this game\n", friends.size());
            if (friends.empty()) {
                Flash("None of your Steam friends are playing TLOU2 right now");
                return;
            }
            g_searching = true;
            g_searchUntil = g_clock + 20.0f;
            g_nextQuery = 0.0f;
            Flash("Looking for a friend who is hosting...");
        }

        void LobbyTick()
        {
            if (!net::steam::Available()) return;
            const bool hosting = net::session::IsRunning() && net::session::IsHost();

            char msg[16];
            uint64_t from = 0;
            for (int guard = 0; guard < 32; ++guard) {
                const int n = net::steam::Recv(net::steam::kChannelLobby, msg, sizeof(msg), &from);
                if (n < 0) break;
                if (n < 1 || !from) continue;
                if (msg[0] == 'Q' && hosting) {
                    const char h = 'H';
                    net::steam::Send(from, &h, 1, net::steam::kChannelLobby);
                    TO1_LOG("[coop] %s (%llu) asked who is hosting -- answered\n", net::steam::FriendName(from),
                        static_cast<unsigned long long>(from));
                }
                else if (msg[0] == 'H' && g_searching) {
                    g_searching = false;
                    char line[160];
                    snprintf(line, sizeof(line), "Joining %s...", net::steam::FriendName(from));
                    Flash(line);
                    TO1_LOG("[coop] joining Steam friend %s (%llu)\n", net::steam::FriendName(from),
                        static_cast<unsigned long long>(from));
                    net::session::JoinGameSteam(from);
                }
            }

            if (hosting && g_clock >= g_nextBeacon) {
                g_nextBeacon = g_clock + 2.0f;
                SendToFriendsInGame('H');
            }
            if (g_searching) {
                if (g_clock >= g_nextQuery) { g_nextQuery = g_clock + 1.0f; SendToFriendsInGame('Q'); }
                if (g_clock > g_searchUntil) {
                    g_searching = false;
                    Flash("No friend is hosting - ask them to press F7 first");
                    TO1_LOG("[coop] F8: no hosting friend answered in 20 s\n");
                }
            }
        }

        void StopSession()
        {
            if (net::session::IsRunning()) {
                TO1_LOG("[coop] F6: leaving the session\n");
                net::session::Leave();
            }
            multiplayer = false;
            coop::enemies::Reset();
            coop::world::Reset();
            g_where = WhereState{};
            g_hostFriendlyFire = false;
            if (g_guestOwnDifficulty >= 0) {                // put our own difficulty back
                if (ENTITY::DOES_EXIST(SID("player"))) DC::Call<"set-difficulty">(static_cast<uint64_t>(g_guestOwnDifficulty));
                TO1_LOG("[coop] difficulty restored to our own %d\n", g_guestOwnDifficulty);
                g_guestOwnDifficulty = -1;
            }
            if (g_hooksOn) { net::hooks::RemoveMultiplayerHooks(); g_hooksOn = false; }
        }

        bool GameFocused()
        {
            HWND fg = GetForegroundWindow();
            if (!fg) return false;
            DWORD pid = 0;
            GetWindowThreadProcessId(fg, &pid);
            return pid == GetCurrentProcessId();
        }

        bool Pressed(int vk)
        {
            static bool down[256] = {};
            const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
            const bool edge = now && !down[vk & 0xFF];
            down[vk & 0xFF] = now;
            return edge;
        }

        // One-line on-screen message under the status line (F10 results etc.), shown for a few seconds.
        char  g_flash[160] = {};
        float g_flashUntil = 0.0f;
        void Flash(const char* msg)
        {
            snprintf(g_flash, sizeof(g_flash), "%s", msg);
            g_flashUntil = g_clock + 5.0f;
        }

        // ---------------------------------------------------------------- solo test puppet (F10)
        // Spawns an inert copy of the local player 3 m ahead, through exactly the same spawner /
        // look path the partner puppet uses -- lets one person check M1 spawning without a
        // second PC. F10 again removes it.
        StringId64 g_testPuppet = 0;

        void ToggleTestPuppet()
        {
            if (g_testPuppet) {
                if (ENTITY::DOES_EXIST(g_testPuppet)) ENTITY::KILL_ENTITY(g_testPuppet);
                TO1_LOG("[coop] F10: test puppet removed\n"); Flash("F10: test puppet removed");
                g_testPuppet = 0;
                return;
            }
            EnsureLocalIdentity();
            const Transform t = LOCATOR::GET_TRANSFORM(SID("player"));
            const Vector3 pos = LOCATOR::GET_POSITION_IN_FRONT_OF(SID("player"), 3.0f);
            const StringId64 npc = SpawnPartnerNpc(pos, t.Rotation, net::session::LocalLook());
            if (!npc) { TO1_LOG("[coop] F10: test puppet spawn FAILED\n"); Flash("F10: test spawn failed - no usable spawner found (see tlou2-coop.log)"); return; }

            DC::Call<"npc-set-script-mode">(npc, 0x2, 0x1);   // same inert setup as NetPlayer::Spawn
            DC::Call<"npc-set-ignore">(npc, 1ull);
            DC::Call<"set-object-invisible-to-ai">(npc, 1ull);
            DC::Call<"npc-set-blind">(npc, 1ull);
            DC::Call<"npc-set-deaf">(npc, 1ull);
            NPC::SET_FACTION_ID(npc, Faction::Buddy);
            g_testPuppet = npc;
            TO1_LOG("[coop] F10: test puppet spawned %016llX\n", static_cast<unsigned long long>(npc)); Flash("F10: test puppet spawned 3 m ahead (F10 again removes it)");
        }

        // ---------------------------------------------------------------- F11: npc-move-to sweep
        // npc-move-to(npc, location, int32, int32, symbol, bool, bool, symbol) -- the argument
        // types come from the engine's readers, their meaning does not. F11 runs every variant
        // below on the F10 test Ellie (4 s each): she is put back 3 m ahead, sent ~6 m further,
        // and how far she actually moved after 1 s and 3 s is logged. Walking shows up as a
        // steady distance, running as a bigger one, a refused variant as ~0.
        struct MoveVariant { int32_t a, b; const char* s1; bool b1, b2; const char* s2; };
        const MoveVariant kMoveVariants[] = {
            { 0, 0, nullptr, false, false, nullptr },
            { 1, 0, nullptr, false, false, nullptr },
            { 2, 0, nullptr, false, false, nullptr },
            { 3, 0, nullptr, false, false, nullptr },
            { 0, 1, nullptr, false, false, nullptr },
            { 0, 2, nullptr, false, false, nullptr },
            { 0, 0, "walk",  false, false, nullptr },
            { 0, 0, "run",   false, false, nullptr },
            { 0, 0, nullptr, false, false, "walk" },
            { 0, 0, nullptr, false, false, "run" },
            { 0, 0, nullptr, true,  false, nullptr },
            { 0, 0, nullptr, false, true,  nullptr },
        };
        constexpr int kNumMoveVariants = sizeof(kMoveVariants) / sizeof(kMoveVariants[0]);
        int     g_sweepIdx = -1;                           // -1 = not running
        float   g_sweepAt = 0.0f;
        bool    g_sweepLogged1 = false;
        Vector3 g_sweepStart{};

        float Dist(const Vector3& a, const Vector3& b)
        {
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        void StartMoveVariant(int i)
        {
            const MoveVariant& v = kMoveVariants[i];
            const Transform pt = LOCATOR::GET_TRANSFORM(SID("player"));
            const Vector3 start = LOCATOR::GET_POSITION_IN_FRONT_OF(SID("player"), 3.0f);
            const Vector3 goal = LOCATOR::GET_POSITION_IN_FRONT_OF(SID("player"), 9.0f);
            DC::Call<"npc-teleport-to">(g_testPuppet, MEMORY::ALLOC_BOUND_FRAME(start, pt.Rotation));
            const uint64_t rc = DC::Call<"npc-move-to">(g_testPuppet, MEMORY::ALLOC_BOUND_FRAME(goal, pt.Rotation),
                static_cast<uint64_t>(static_cast<uint32_t>(v.a)), static_cast<uint64_t>(static_cast<uint32_t>(v.b)),
                v.s1 ? ToStringId64(v.s1) : 0ull, v.b1 ? 1ull : 0ull, v.b2 ? 1ull : 0ull, v.s2 ? ToStringId64(v.s2) : 0ull);
            g_sweepStart = LOCATOR::GET_TRANSFORM(g_testPuppet).Position;
            g_sweepAt = g_clock;
            g_sweepLogged1 = false;
            TO1_LOG("[movetest] variant %d: a=%d b=%d s1=%s b1=%d b2=%d s2=%s -> rc %llu\n", i, v.a, v.b,
                v.s1 ? v.s1 : "-", v.b1, v.b2, v.s2 ? v.s2 : "-", static_cast<unsigned long long>(rc));
            char line[96];
            snprintf(line, sizeof(line), "F11 move test: variant %d of %d", i + 1, kNumMoveVariants);
            Flash(line);
        }

        void TickMoveSweep()
        {
            if (g_sweepIdx < 0) return;
            if (!g_testPuppet || !ENTITY::DOES_EXIST(g_testPuppet)) {
                TO1_LOG("[movetest] stopped: the F10 test Ellie is gone\n");
                g_sweepIdx = -1;
                return;
            }
            const float t = g_clock - g_sweepAt;
            const float moved = Dist(LOCATOR::GET_TRANSFORM(g_testPuppet).Position, g_sweepStart);
            if (!g_sweepLogged1 && t >= 1.0f) { g_sweepLogged1 = true; TO1_LOG("[movetest] variant %d: moved %.2f m after 1 s\n", g_sweepIdx, moved); }
            if (t >= 3.0f) {
                TO1_LOG("[movetest] variant %d: moved %.2f m after 3 s\n", g_sweepIdx, moved);
                if (++g_sweepIdx >= kNumMoveVariants) {
                    g_sweepIdx = -1;
                    TO1_LOG("[movetest] sweep done\n");
                    Flash("F11 move test done - close the game or press F10 to remove her");
                    return;
                }
                StartMoveVariant(g_sweepIdx);
            }
        }

        // ---------------------------------------------------------------- F2: enemies ignore you
        // Solo testing aid: every enemy is made blind and deaf (the engine's npc-set-blind /
        // npc-set-deaf, as on the partner's enemy copies), re-applied every second so enemies
        // that spawn later are covered too. F2 again gives them their senses back. Followers
        // (the partner's enemy copies) are left alone: they are blind and deaf on purpose.
        bool g_aiIgnore = false;
        float g_nextAiIgnore = 0.0f;
        std::vector<StringId64> g_aiIgnored;              // enemies this switched off, to switch back on

        bool IsFollower(StringId64 npc)
        {
            const uintptr_t p = static_cast<uintptr_t>(DcManager::LookupObject(npc));
            return p && coop::enemies::IsFollowerPtr(p);
        }

        void SetAiIgnore(bool on)
        {
            g_aiIgnore = on;
            g_nextAiIgnore = 0.0f;
            if (!on) {
                int n = 0;
                for (StringId64 npc : g_aiIgnored)
                    if (ENTITY::DOES_EXIST(npc) && !IsFollower(npc)) {
                        DC::Call<"npc-set-blind">(npc, 0ull);
                        DC::Call<"npc-set-deaf">(npc, 0ull);
                        ++n;
                    }
                g_aiIgnored.clear();
                TO1_LOG("[test] F2: enemies can see and hear you again (%d restored)\n", n);
                Flash("F2: enemies can see you again");
            }
            else {
                TO1_LOG("[test] F2: enemies ignore you (blind + deaf)\n");
                Flash("F2: enemies ignore you (F2 again to undo)");
            }
        }

        void TickAiIgnore()
        {
            if (!g_aiIgnore || g_clock < g_nextAiIgnore) return;
            g_nextAiIgnore = g_clock + 1.0f;
            if (!ENTITY::DOES_EXIST(SID("player"))) return;
            for (StringId64 npc : FEDRA::GET_ALL_ENEMIES()) {
                if (!npc || !ENTITY::DOES_EXIST(npc) || IsFollower(npc)) continue;
                DC::Call<"npc-set-blind">(npc, 1ull);
                DC::Call<"npc-set-deaf">(npc, 1ull);
                if (std::find(g_aiIgnored.begin(), g_aiIgnored.end(), npc) == g_aiIgnored.end()) g_aiIgnored.push_back(npc);
            }
        }

        // ---------------------------------------------------------------- boot_task (testing)
        // boot_task=<node>/<task> in tlou2-coop.ini: a few seconds after the main menu is up, the
        // game loads that story task (task-play-debug!, the call menu joining uses). Saving is
        // switched off for that game session so the test cannot overwrite the campaign autosave.
        void TickBootTask()
        {
            static bool done = false;
            static float menuSince = -1.0f;
            static StringId64 lastPage = ~0ull;
            if (done || g_cfg.bootTask.empty()) return;
            if (ENTITY::DOES_EXIST(SID("player")) || net::session::IsRunning()) { done = true; return; }   // already playing / joining
            // Wait for the main menu: a page on the main menu stack, steady for 3 s (logos and
            // the title screen come and go before it).
            const StringId64 page = MENU2::GET_TOP_PAGE_IN_STACK(MenuStack::main);
            if (page != lastPage) {
                lastPage = page;
                menuSince = page ? g_clock : -1.0f;
                const char* n = SidName(page);
                TO1_LOG("[test] boot_task: main menu page %016llX \"%s\"\n", static_cast<unsigned long long>(page), n ? n : "?");
            }
            if (menuSince < 0.0f || g_clock - menuSince < 3.0f) return;
            done = true;
            g_testBlockSaves = true;
            TO1_LOG("[test] boot_task: loading %s / %s (saving off for this session)\n", g_cfg.bootNode.c_str(), g_cfg.bootTask.c_str());
            Flash("Test boot: loading the test area (saving off)");
            PlayTaskFromMod(ToStringId64(g_cfg.bootNode.c_str()), ToStringId64(g_cfg.bootTask.c_str()));
        }

        // Runs the discovery probe once per level, a few seconds after the player appears, so a
        // test run's log always has the spawner list without anyone pressing F9.
        void AutoProbe()
        {
            static float playerSince = -1.0f;
            static bool probed = false;
            if (!ENTITY::DOES_EXIST(SID("player"))) { playerSince = -1.0f; probed = false; g_testPuppet = 0; return; }
            if (playerSince < 0.0f) playerSince = g_clock;
            // The full spawner dump (a short hitch, ~400 log lines) only while a co-op session
            // runs; playing alone it is F9 only.
            if (!probed && multiplayer && g_clock - playerSince > 5.0f) {
                probed = true;
                ProbeAroundPlayer();
            }
        }

        // ---------------------------------------------------------------- No Return discovery (NR1)
        // The game world of the current task ([[task]+0x120]+0x18): "world-rogue" in No Return.
        StringId64 CurrentWorld()
        {
            const uintptr_t t = static_cast<uintptr_t>(DC::Call<"task-get-current">());
            if (!PlausiblePtr(t)) return 0;
            const uintptr_t node = ReadPtr(t + 0x120);
            uint64_t w = 0;
            return (node && SafeRead(node + 0x18, &w, 8)) ? w : 0;
        }

        // The exe's executable section: a script symbol whose function slot points in here is a
        // native (C++) script function, callable like DC::Call does.
        bool InExeCode(uintptr_t a)
        {
            static uintptr_t lo = 0, hi = 0;
            if (!lo) {
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
                const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
                for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
                    if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
                        lo = g_base + sec[i].VirtualAddress;
                        hi = lo + sec[i].Misc.VirtualSize;
                        break;
                    }
            }
            return a >= lo && a < hi;
        }

        // The function slot of a script symbol (0 if the symbol does not exist).
        uintptr_t ScriptFunctionSlot(StringId64 sid)
        {
            const DcValue* v = DcManager::Lookup(sid);
            uint64_t f = 0;
            if (!v || !SafeRead(reinterpret_cast<uintptr_t>(&v->val[SCRIPT_FUNC_INDEX]), &f, 8)) return 0;
            return f ? f : 1;                              // exists, but no function in that slot
        }

        constexpr const char* kNoReturnNames[] = {
#include "coop/nr-names.inc"
        };

        // Questions about the run that take no arguments, asked once a second in No Return and
        // logged when the answer changes: what an encounter / the hideout / a run end look like.
        constexpr const char* kNoReturnWatch[] = {
            "in-rogue-mode?", "is-rogue-mode?", "is-rogue-like?", "rogue-player-loaded?", "rogue-player-set?",
            "rogue-player-in-hub?", "rogue-player-in-encounter?", "rogue-run-abandoned?",
            "is-rogue-in-post-encounter-flow?", "is-rogue-hub-waiting-on-post-encounter-flow?",
            "is-encounter-select?", "is-encounter-restart?", "is-boss-encounter?", "is-defense-encounter?",
            "is-human-faction-encounter?", "is-infected-faction-encounter?", "encounter-over?", "failed-encounter?",
            "defense-wave-active?", "assault-wave-active?", "last-npc-in-rogue-wave?", "is-speed-run-mode-enabled?",
            // NR1b: the run itself -- seeds, character, layout (natives found by the M3.4 scan).
            // Values that are string ids are logged with their names.
            "rogue-get-primary-seed", "rogue-get-daily-run-seed", "custom-run--using-override-seed?",
            "rogue-get-player-character", "rogue-get-player-character-look", "rogue-get-most-recent-character",
            "rogue-get-overall-mode", "rogue-get-hub-node", "rogue-get-hub-subnode", "rogue-get-is-resumed-run",
            "rogue-get-run-template", "rogue-get-run-structure", "rogue-get-custom-run", "get-speed-run-mode",
            "task-played-from-encounter-selection?",
        };

        // The last No Return encounter seen this session (Shift+F11 loads it from the hideout).
        TaskId g_lastEncounter{};

        void TickNoReturnProbe()
        {
            // Once per game session: which of the No Return names are native script functions.
            static bool scanned = false;
            if (!scanned) {
                scanned = true;
                int natives = 0, others = 0;
                TO1_LOG("[nr] ===== No Return name scan: %zu names =====\n", std::size(kNoReturnNames));
                for (const char* n : kNoReturnNames) {
                    const uintptr_t f = ScriptFunctionSlot(ToStringId64(n));
                    if (!f) continue;
                    if (InExeCode(f)) {
                        ++natives;
                        TO1_LOG("[nr] native %s  exe+%llX\n", n, static_cast<unsigned long long>(f - g_base));
                    }
                    else {
                        ++others;
                        TO1_LOG("[nr] symbol %s  (%016llX)\n", n, static_cast<unsigned long long>(f));
                    }
                }
                TO1_LOG("[nr] ===== %d native functions, %d other symbols =====\n", natives, others);
            }

            // The watch list: natives only (a script-defined function cannot be called this way).
            struct Watch { const char* name; uintptr_t fn; uint64_t last; bool called; int logs; };
            static std::vector<Watch> watch;
            static bool built = false;
            if (!built) {
                built = true;
                for (const char* n : kNoReturnWatch) {
                    const uintptr_t f = ScriptFunctionSlot(ToStringId64(n));
                    if (f && InExeCode(f)) watch.push_back({ n, f, 0, false, 0 });
                    else TO1_LOG("[nr] watch: %s is not a native, skipped\n", n);
                }
            }
            using NativeFn = DcValue (*)(int, DcValue*, uint64_t);
            for (Watch& w : watch) {
                // The first call of each is logged first: if one of them crashes the game, the
                // log's last line names it.
                if (!w.called) TO1_LOG("[nr] first call: %s\n", w.name);
                DcValue args{};
                const uint64_t v = reinterpret_cast<NativeFn>(w.fn)(0, &args, 0).val[0];
                // (at most 12 changes each: a value that is a fresh pointer every call would
                // flood the log)
                if ((!w.called || v != w.last) && w.logs < 12) {
                    ++w.logs;
                    const char* n = SidName(v);
                    TO1_LOG("[nr] %s = %llX%s%s%s%s\n", w.name, static_cast<unsigned long long>(v),
                        n ? " \"" : "", n ? n : "", n ? "\"" : "", w.logs == 12 ? "  (no more changes logged)" : "");
                }
                w.called = true;
                w.last = v;
            }
        }

        // Where the game is, in the log whenever it changes (campaign chapter part / task, No
        // Return hideout / encounter): one line per change, plus the task object's names when
        // the part (node) changes. Cheap (one script call a second), runs solo too -- it is
        // how the No Return modes get mapped.
        void LogWhereWeAre()
        {
            static float next = 0.0f;
            static TaskId last{};
            if (g_clock < next) return;
            next = g_clock + 1.0f;
            if (!ENTITY::DOES_EXIST(SID("player"))) return;
            TaskId cur;
            if (!CurrentTask(cur)) return;
            const bool noReturn = CurrentWorld() == SID("world-rogue");
            if (noReturn) TickNoReturnProbe();
            if (cur.node == last.node && cur.task == last.task && cur.day == last.day) return;
            const bool newNode = cur.node != last.node;
            last = cur;
            auto name = [](StringId64 s) { const char* n = SidName(s); return n ? n : "?"; };
            TO1_LOG("[where] node %016llX \"%s\"  task %016llX \"%s\"  day %016llX \"%s\"  difficulty %llu%s\n",
                static_cast<unsigned long long>(cur.node), name(cur.node),
                static_cast<unsigned long long>(cur.task), name(cur.task),
                static_cast<unsigned long long>(cur.day), name(cur.day),
                static_cast<unsigned long long>(DC::Call<"get-difficulty">()), noReturn ? "  [No Return]" : "");
            if (newNode || noReturn) ProbeTask();          // No Return: every task (hideout, each encounter)
            if (noReturn && cur.node != SID("t2r-hub-proto")) g_lastEncounter = cur;
        }

        // Name tags: the partner's name centred 2.1 m above its puppet. Screen space is the
        // engine's virtual 1280x720 (world-space-to-screen-space, the same space DrawTextPrintV
        // uses). Hidden behind the camera (camera +Z is forward) and beyond 80 m.
        void DrawNameTagFor(StringId64 npc, const char* name, const Vector3& camPos, float fx, float fy, float fz)
        {
            if (!npc || !ENTITY::DOES_EXIST(npc)) return;
            Vector3 head = LOCATOR::GET_TRANSFORM(npc).Position;
            head.y += 2.1f;
            const float dx = head.x - camPos.x, dy = head.y - camPos.y, dz = head.z - camPos.z;
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist > 80.0f || dx * fx + dy * fy + dz * fz <= 0.0f) return;
            const Vector2 s = SYSTEM::WORLD_TO_SCREEN(head);
            if (!std::isfinite(s.x) || !std::isfinite(s.y) || s.x < 0.0f || s.x > 1280.0f || s.y < 0.0f || s.y > 720.0f) return;
            const float scale = dist < 15.0f ? 0.7f : 0.55f;
            const vec2 sz = DebugDrawCommon::GetTextWidthHeight(name, scale);
            DebugDrawCommon::DrawTextPrintV(s.x - sz.x * 0.5f, s.y - sz.y, scale, _rgb(255, 255, 255), "%s", name);
        }

        void DrawNameTags()
        {
            if (!g_cfg.nametags || !ENTITY::DOES_EXIST(SID("player"))) return;
            const bool connected = net::session::IsConnected();
            if (!connected && !g_testPuppet) return;
            const uintptr_t camLoc = CAMERA::GET_CAMERA_LOCATOR();
            if (!camLoc) return;
            const Vector3 camPos = LOCATOR::GET_LOCATOR_POSITION(camLoc);
            const Quaternion cq = LOCATOR::GET_LOCATOR_ROTATION(camLoc);
            // rotate (0,0,1) by the camera quaternion
            const float fx = 2.0f * (cq.x * cq.z + cq.w * cq.y);
            const float fy = 2.0f * (cq.y * cq.z - cq.w * cq.x);
            const float fz = 1.0f - 2.0f * (cq.x * cq.x + cq.y * cq.y);

            if (connected)
                for (NetPlayer& p : NetPlayers::Remotes())
                    if (p.spawned) DrawNameTagFor(p.npcSid, p.name[0] ? p.name : "Partner", camPos, fx, fy, fz);

            // The F10 test Ellie gets a tag too (our own name), so tags can be checked solo.
            if (g_testPuppet) DrawNameTagFor(g_testPuppet, net::session::LocalName(), camPos, fx, fy, fz);
        }

        void DrawOverlay()
        {
            DrawNameTags();
            if (!g_cfg.overlay) return;
            char line[256];
            const char* state = g_searching ? "looking for a hosting friend..." : "idle";
            if (net::session::IsRunning()) {
                if (net::session::IsConnected()) state = net::session::IsHost() ? "hosting, partner connected" : "connected to host";
                else state = net::session::IsHost() ? "hosting, waiting for partner" : "joining...";
            }
            snprintf(line, sizeof(line), "CO-OP: %s%s   [F7 host  F8 join%s%s  F6 leave]", state,
                g_blockSaves.load(std::memory_order_relaxed) ? " - saving off" : "",
                g_cfg.joinIp.empty() ? "" : " ", g_cfg.joinIp.c_str());
            DebugDrawCommon::DrawTextPrintV(20.0f, 20.0f, 0.6f, _rgb(235, 235, 235), "%s", line);
            if (g_flash[0] && g_clock < g_flashUntil)
                DebugDrawCommon::DrawTextPrintV(20.0f, 40.0f, 0.6f, _rgb(255, 210, 120), "%s", g_flash);
        }
    }

    void Tick()
    {
        static bool init = false;
        if (!init) {
            init = true;
            LoadConfig();
            InstallLevelMgrCapture();
            InstallSaveBlock();
            InstallRogueSeedHooks();
            net::hooks::InstallMeleeProbe();
            TO1_LOG("[coop] ready. F7 host, F8 join, F6 leave, F9 discovery probe\n");
        }

        const float dt = TIME::GET_DELTA_TIME();
        if (dt > 0.0f && dt < 1.0f) g_clock += dt;

        if (GameFocused()) {
            if (Pressed(VK_F7)) {
                if (net::session::IsRunning() && net::session::IsHost())
                    Flash("Already hosting - your Steam friend presses F8 to join (F6 stops)");
                else
                    StartSession(Want::Host);
            }
            if (Pressed(VK_F8)) StartSession(Want::Join);
            if (Pressed(VK_F2)) SetAiIgnore(!g_aiIgnore);   // testing: enemies ignore you
            if (Pressed(VK_F6)) {
                // Shift+F6 (solo test): the nearest enemy takes a "partner melee hit" from the F10
                // test Ellie, through the path a real partner's melee hit takes.
                if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) && ENTITY::DOES_EXIST(SID("player"))) {
                    Flash(g_testPuppet ? "Shift+F6: partner melee hit test on the nearest enemy"
                                       : "Shift+F6: hit test (press F10 first so the hit comes from the test Ellie)");
                    coop::enemies::TestPartnerHit(g_testPuppet);
                }
                else StopSession();
            }
            if (Pressed(VK_F9) && ENTITY::DOES_EXIST(SID("player"))) {
                if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
                    // Shift+F9: reload the current task through task-play-debug! -- the exact
                    // call a joining partner makes -- so the load path can be checked solo.
                    TaskId id;
                    if (CurrentTask(id)) {
                        TO1_LOG("[coop] Shift+F9: task-play-debug! %016llX %016llX\n",
                            static_cast<unsigned long long>(id.node), static_cast<unsigned long long>(id.task));
                        Flash("Shift+F9: reloading the current task (join test)");
                        DEBUG::PLAY_TASK(id.node, id.task, 0);
                    }
                    else Flash("Shift+F9: no current task found");
                }
                else ProbeAroundPlayer();
            }
            if (Pressed(VK_F11) && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
                // Shift+F11 (No Return test): from the hideout, load the last encounter played
                // this session through task-play-debug! -- what a joining partner's game would
                // do to follow the host into an encounter.
                TaskId cur;
                if (CurrentWorld() != SID("world-rogue")) Flash("Shift+F11: No Return only");
                else if (!g_lastEncounter.task) Flash("Shift+F11: play one encounter first, then come back to the hideout");
                else if (!CurrentTask(cur) || cur.node != SID("t2r-hub-proto")) Flash("Shift+F11: go back to the hideout first");
                else {
                    TO1_LOG("[nr] Shift+F11: loading the last encounter %016llX \"%s\" / %016llX \"%s\" from the hideout\n",
                        static_cast<unsigned long long>(g_lastEncounter.node), SidName(g_lastEncounter.node) ? SidName(g_lastEncounter.node) : "?",
                        static_cast<unsigned long long>(g_lastEncounter.task), SidName(g_lastEncounter.task) ? SidName(g_lastEncounter.task) : "?");
                    Flash("Shift+F11: loading the last encounter (No Return join test)");
                    DEBUG::PLAY_TASK(g_lastEncounter.node, g_lastEncounter.task, 0);
                }
            }
            else if (Pressed(VK_F11)) {
                if (!g_testPuppet || !ENTITY::DOES_EXIST(g_testPuppet)) Flash("F11: press F10 first to spawn the test Ellie");
                else if (g_sweepIdx < 0) { TO1_LOG("[movetest] sweep start\n"); g_sweepIdx = 0; StartMoveVariant(0); }
            }
            if (Pressed(VK_F10)) {
                if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
                    // Shift+F10: force the guest save block on / off, to test it without a partner.
                    g_testBlockSaves = !g_testBlockSaves;
                    TO1_LOG("[coop] Shift+F10: test save block %s\n", g_testBlockSaves ? "ON" : "OFF");
                    Flash(g_testBlockSaves ? "Shift+F10: saving OFF (test) - try a save now" : "Shift+F10: saving back ON");
                }
                else if (ENTITY::DOES_EXIST(SID("player"))) ToggleTestPuppet();
            }
        }

        AutoProbe();
        LogWhereWeAre();
        TickAiIgnore();
        TickCloseMenuAfterLoad();
        TickBootTask();
        TickMoveSweep();
        LobbyTick();
        coop::world::Tick(TIME::GET_DELTA_TIME());
        TickWhereHost();
        TickWhereClient();
        TickGuestLife();

        // Guest save block: on while joined to someone else's game (or forced for testing).
        const bool guest = net::session::IsRunning() && !net::session::IsHost();
        const bool block = guest || g_testBlockSaves;
        if (block != g_blockSaves.load(std::memory_order_relaxed)) {
            g_blockSaves.store(block, std::memory_order_relaxed);
            TO1_LOG("[coop] saving %s\n", !block ? "back ON" : guest ? "OFF (guest in the host's game)" : "OFF (test session)");
        }
        static int s_shownBlocked = 0;
        const int blocked = g_savesBlocked.load(std::memory_order_relaxed);
        if (blocked != s_shownBlocked) {
            s_shownBlocked = blocked;
            Flash("Save skipped - you are in the host's game, your own save is safe");
        }

        // F8's Steam search runs before any session exists -- that is not "the session ended".
        if (multiplayer && !net::session::IsRunning() && g_hooksOn && !g_searching) {
            // the session ended on its own (timeout / host left): drop back to plain campaign
            TO1_LOG("[coop] session ended\n");
            StopSession();
        }

        DrawOverlay();
    }

    void OnWhere(uint8_t senderSlot, const uint8_t* data, uint8_t size)
    {
        if (senderSlot != 0 || net::session::IsHost() || size < sizeof(CoopWhereEvent)) return;   // host is slot 0
        CoopWhereEvent e;
        memcpy(&e, data, sizeof(e));
        if (!e.node || !e.task) return;
        if (!g_where.have || e.node != g_where.w.node) {
            g_where.loadIssued = false;                    // a new chapter part: load it once more
            g_where.placed = false;
        }
        if (g_where.lifeKnown && e.life != g_where.life) {
            g_where.forceReload = true;                    // the host died / reloaded a checkpoint
            TO1_LOG("[coop] host life %u -> %u (died or reloaded)\n", g_where.life, e.life);
        }
        g_where.life = e.life;
        g_where.lifeKnown = true;

        // Difficulty sync: play on the host's difficulty; our own is put back when we leave
        // (StopSession). Our saves are blocked while joined, so it cannot overwrite our setting.
        if (e.difficulty != 0xFF && ENTITY::DOES_EXIST(SID("player"))) {
            const uint64_t mine = DC::Call<"get-difficulty">();
            if (mine != e.difficulty && mine <= 0xFE) {
                if (g_guestOwnDifficulty < 0) g_guestOwnDifficulty = static_cast<int>(mine);
                DC::Call<"set-difficulty">(static_cast<uint64_t>(e.difficulty));
                TO1_LOG("[coop] difficulty %llu -> host's %u\n", static_cast<unsigned long long>(mine), e.difficulty);
                Flash("Difficulty set to the host's");
            }
        }
        const bool ff = (e.flags & kCoopWhereFriendlyFire) != 0;
        if (ff != g_hostFriendlyFire.exchange(ff, std::memory_order_relaxed))
            TO1_LOG("[coop] host's friendly fire: %s\n", ff ? "on" : "off");
        g_where.w = e;                                     // the newest host position is used for placing
        g_where.have = true;
    }

    bool IsPlacedWithHost() { return g_where.placed; }

    bool FriendlyFire()
    {
        return net::session::IsHost() ? g_cfg.friendlyFire : g_hostFriendlyFire.load(std::memory_order_relaxed);
    }

    // Pickups = spawners of the same process type as any spawner named "item-pickup-*",
    // "coll-*" or "collectible-*". Reading every spawner's name is the slow part, so each
    // level's (name sid, type) list is kept until that level unloads: a rebuild only reads the
    // levels that streamed in since the last one (Seattle's open world streams often).
    void PickupSpawnerSids(std::vector<StringId64>& out)
    {
        struct LevelCache { uintptr_t arr = 0; int32_t n = 0; std::vector<std::pair<StringId64, StringId64>> sps; };
        static std::unordered_map<uintptr_t, LevelCache> s_levels;   // level -> its spawners
        static std::vector<StringId64> s_pickupTypes;                // grows; process types are fixed

        std::vector<uintptr_t> levels;
        LoadedLevels(levels);
        std::unordered_map<uintptr_t, LevelCache> keep;
        size_t read = 0;
        for (uintptr_t lvl : levels) {
            // The same level is the same pointer with the same spawner array; a freed level's
            // memory reused by another one shows a different array or count.
            uintptr_t arr = 0; int32_t n = 0;
            if (const uintptr_t sd = ReadPtr(lvl + kLvlSpawnData)) { arr = ReadPtr(sd + kSdArray); SafeRead(sd + kSdCount, &n, 4); }
            auto it = s_levels.find(lvl);
            if (it != s_levels.end() && it->second.arr == arr && it->second.n == n) { keep[lvl] = std::move(it->second); continue; }
            LevelCache c; c.arr = arr; c.n = n;
            std::vector<uintptr_t> sps;
            LevelSpawners(lvl, sps);
            for (uintptr_t sp : sps) {
                const StringId64 sid = SpawnerSid(sp), type = SpawnerProcessType(sp);
                if (!sid || !type) continue;
                c.sps.emplace_back(sid, type);
                if (std::find(s_pickupTypes.begin(), s_pickupTypes.end(), type) != s_pickupTypes.end()) continue;
                const std::string name = SpawnerName(sp);
                ++read;
                if (name.rfind("item-pickup-", 0) == 0 || name.rfind("coll-", 0) == 0 || name.rfind("collectible-", 0) == 0)
                    s_pickupTypes.push_back(type);
            }
            keep[lvl] = std::move(c);
        }
        s_levels = std::move(keep);
        for (const auto& [lvl, c] : s_levels)
            for (const auto& [sid, type] : c.sps)
                if (std::find(s_pickupTypes.begin(), s_pickupTypes.end(), type) != s_pickupTypes.end()) out.push_back(sid);
        if (read) TO1_LOG("[coop] pickup scan: read %zu new spawner names, %zu pickup types known\n", read, s_pickupTypes.size());
    }

    uint32_t LoadedLevelCount()
    {
        std::vector<uintptr_t> levels;
        LoadedLevels(levels);
        return static_cast<uint32_t>(levels.size());
    }

    namespace {
        std::atomic<uintptr_t> g_guestHealthComp{ 0 };
        std::atomic<bool>      g_guestDowned{ false };
    }

    uintptr_t GuestHealthComponent() { return g_guestHealthComp.load(std::memory_order_relaxed); }
    void NoteGuestDowned() { g_guestDowned.store(true, std::memory_order_relaxed); }

    // Main thread, every frame: publish the guest's health component for the damage hook, and
    // turn a "would have died" hit into a respawn beside the host (full health, no reload, so
    // the guest's world -- enemies, doors, pickups -- stays exactly as it was).
    void TickGuestLife()
    {
        const bool guest = net::session::IsRunning() && !net::session::IsHost() && g_where.have;
        const uintptr_t proc = guest ? static_cast<uintptr_t>(DcManager::LookupObject(SID("player"))) : 0;
        g_guestHealthComp.store(proc ? ReadPtr(proc + 0x610) : 0, std::memory_order_relaxed);

        if (!g_guestDowned.exchange(false, std::memory_order_relaxed) || !proc) return;
        const Vector3 p{ g_where.w.pos[0] + 1.2f, g_where.w.pos[1], g_where.w.pos[2] };
        const Quaternion q{ g_where.w.rot[0], g_where.w.rot[1], g_where.w.rot[2], g_where.w.rot[3] };
        TeleportPlayer(p, q);
        PLAYER::SET_HEALTH(PLAYER::GET_MAX_HEALTH());
        TO1_LOG("[coop] guest took a killing hit -> respawned beside the host at full health (no reload)\n");
        Flash("You went down - respawned next to your partner");
    }

    void OnPuppetDamaged(int ownerSlot, float amount)
    {
        if (ownerSlot < 0 || ownerSlot >= kMaxNetPlayers || !(amount > 0.0f) || amount > 1.0e5f) return;
        CoopHurtEvent e{ amount };
        net::session::SendEvent(NEVT_COOP_HURT, e, static_cast<uint8_t>(ownerSlot));   // the event ring is thread safe
        TO1_LOG("[coop] partner slot %d hit here for %.1f -> sent to them\n", ownerSlot, amount);
    }

    void OnHurt(uint8_t senderSlot, const uint8_t* data, uint8_t size)
    {
        if (size < sizeof(CoopHurtEvent)) return;
        CoopHurtEvent e;
        memcpy(&e, data, sizeof(e));
        if (!(e.amount > 0.0f) || e.amount > 1.0e5f) return;
        const uintptr_t me = static_cast<uintptr_t>(DcManager::LookupObject(SID("player")));
        if (!me) return;
        // Through the engine's own damage path (hit reaction, death when it runs out), like a
        // bullet from the enemy that fired it on the other machine.
        const bool ok = net::engine::FireAttack(me, me, SID("pistol-beretta"), 0, e.amount);
        if (!ok) {
            const int h = PLAYER::GET_HEALTH();
            PLAYER::SET_HEALTH((std::max)(0, h - static_cast<int>(e.amount + 0.5f)));
        }
        TO1_LOG("[coop] hit for %.1f in slot %d's game -> applied %s\n", e.amount, senderSlot, ok ? "as an attack" : "to health");
    }

    const char* LocalCharacterLook()
    {
        TaskId id;
        if (CurrentTask(id) && (id.day == SID("Abby Day 1") || id.day == SID("Abby Day 2") || id.day == SID("Abby Day 3")))
            return "abby";
        return "ellie";
    }

    void Notify(const char* msg)
    {
        if (msg && msg[0]) Flash(msg);
    }

    bool CanSpawnPartner()
    {
        if (!ENTITY::DOES_EXIST(SID("player"))) return false;
        if (TIME::IS_GAME_PAUSED()) return false;
        const float retry = (g_spawnFails < 5) ? 3.0f : 15.0f;
        return (g_clock - g_lastSpawnTry) >= retry;
    }

    void OnPartnerSpawnAttempt(bool ok)
    {
        g_lastSpawnTry = g_clock;
        if (ok) g_spawnFails = 0;
        else    ++g_spawnFails;
    }

    StringId64 SpawnPartnerNpc(const Vector3& pos, const Quaternion& rot, const char* look)
    {
        // Multi-spawn: the engine's spawn routine (FUN_140d606d0, behind spawn-entity-at-with-look)
        // checks spawner byte +0x81 bit 0x10. Clear = if the spawner's NPC already exists it is
        // REUSED and moved (that is how the partner used to take over a story NPC); set = a new
        // entity is always created. Only that one bit is set for the call and the byte is put
        // back. Spawners with +0x80 bit 0x08 are refused by the engine and skipped here.
        constexpr uintptr_t kSpFlags0 = 0x80, kSpFlags1 = 0x81;
        constexpr uint8_t   kDisabled = 0x08, kMultiSpawn = 0x10;

        std::vector<SpawnerInfo> cands;
        PuppetSpawnerCandidates(cands);
        if (cands.empty()) {
            TO1_LOG("[coop] partner spawn: no NPC spawner in the loaded levels\n");
            return 0;
        }
        const StringId64 lookSid = (look && look[0]) ? ToStringId64(look) : 0;
        int tried = 0;
        for (const SpawnerInfo& s : cands) {
            uint8_t f0 = 0, f1 = 0;
            if (!SafeRead(s.sp + kSpFlags0, &f0, 1) || !SafeRead(s.sp + kSpFlags1, &f1, 1)) continue;
            if (f0 & kDisabled) continue;
            if (tried++ >= 8) break;

            const bool setBit = !(f1 & kMultiSpawn);
            if (setBit && !SafeWrite8(s.sp + kSpFlags1, static_cast<uint8_t>(f1 | kMultiSpawn))) continue;
            StringId64 npc = lookSid ? ENTITY::SPAWN_ENTITY(s.sid, pos, rot, lookSid) : 0;
            const bool withLook = npc != 0;
            if (!npc) npc = ENTITY::SPAWN_ENTITY(s.sid, pos, rot, 0);
            if (setBit) SafeWrite8(s.sp + kSpFlags1, f1);

            const bool reused = npc != 0 && npc == s.sid;      // the level's own NPC came back, not a copy
            TO1_LOG("[coop] partner spawn try %d: \"%s\" art=\"%s\" look=\"%s\" flags=%02X/%02X -> %016llX%s%s\n", tried,
                s.name.c_str(), s.art.c_str(), look ? look : "", f0, f1, static_cast<unsigned long long>(npc),
                npc ? (withLook ? " (with look)" : " (spawner's own look)") : "",
                reused ? " WARNING: reused the level's own NPC" : (npc ? " (new copy)" : ""));
            if (npc) return npc;
        }
        TO1_LOG("[coop] partner spawn: all %d tries failed (at %.1f %.1f %.1f)\n", tried, pos.x, pos.y, pos.z);
        return 0;
    }
}
