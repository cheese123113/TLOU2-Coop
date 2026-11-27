#include "net-engine.hpp"
#include "net-player.hpp"
#include "net-patches.hpp"
#include "net-debug.hpp"

namespace net::engine {

    const char* KillProcess(uintptr_t process)
    {
        static auto fn = GameFn<const char* (__fastcall*)(uintptr_t)>(0x1341470);
        return fn(process);
    }

    void ForceMeleeAction(uintptr_t player, StringId64 sid)
    {
        if (!player) return;
        uintptr_t ctrl = *reinterpret_cast<uintptr_t*>(player + 0x3108);
        if (!ctrl) return;
        uintptr_t vt = *reinterpret_cast<uintptr_t*>(ctrl);
        uintptr_t fn = *reinterpret_cast<uintptr_t*>(vt + 296);
        reinterpret_cast<void(__fastcall*)(uintptr_t, uint64_t)>(fn)(ctrl, sid);
    }

    void StopMeleeAction(StringId64 entity)
    {
        StringId64 currentMeleeAction = MELEE::GET_CURRENT_MELEE_ACTION(entity);
        if (currentMeleeAction != 0)
            DC::Call<"stop-melee-action">(entity);
    }

    void RequestState(uintptr_t player, uintptr_t descriptor)
    {
        auto fn = GameFn<__int64(*)(uintptr_t, uintptr_t)>(0x1342560);
        fn(player, descriptor);
    }

    StringId64 GetPlayerWeaponId(uintptr_t playerPtr)
    {
        if (playerPtr == 0) return 0;

        uintptr_t v3 = *reinterpret_cast<uintptr_t*>(playerPtr + 12128);
        if (v3 == 0) return 0;

        auto GetWeapon = GameFn<uintptr_t(__fastcall*)(uintptr_t)>(0xBDC1F0);
        uintptr_t v4 = GetWeapon(v3);
        if (v4 == 0) return 0;

        char v7[24];
        auto GetWeaponId = GameFn<uintptr_t(__fastcall*)(uintptr_t, char*)>(0x1163D80);
        uintptr_t result = GetWeaponId(v4, v7);
        return *reinterpret_cast<uint64_t*>(result);
    }

    uintptr_t GetHeldFirearm(uintptr_t process)
    {
        if (!process) return 0;

        const uintptr_t list = *reinterpret_cast<uintptr_t*>(process + 0x3020);
        if (!list) return 0;

        const uint32_t count = *reinterpret_cast<uint32_t*>(list + 0xA0);
        if (!count || count > 64) return 0;

        auto listGetAt = GameFn<uintptr_t(__fastcall*)(uintptr_t, int)>(0x1E236A0);
        auto asWeapon = GameFn<uintptr_t(__fastcall*)(uintptr_t)>(0x1160970);
        auto isFirearm = GameFn<uintptr_t(__fastcall*)(uintptr_t)>(0x11619F0);

        for (uint32_t i = 0; i < count; ++i) {
            const uintptr_t held = listGetAt(list, static_cast<int>(i));
            if (!held) continue;
            const uintptr_t weapon = asWeapon(held);
            if (weapon && isFirearm(weapon)) return weapon;
        }
        return 0;
    }

    bool ArmLaunchVelocity(uintptr_t weapon, const float* vel3)
    {
        if (!weapon || !vel3) return false;

        float lenSq = 0.0f;
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(vel3[i])) return false;
            lenSq += vel3[i] * vel3[i];
        }
        if (lenSq <= 0.0f) return false;

        alignas(16) float vel[4] = { vel3[0], vel3[1], vel3[2], 0.0f };
        memcpy(reinterpret_cast<void*>(weapon + 0xF40), vel, sizeof(vel));
        *reinterpret_cast<uint8_t*>(weapon + 0xF30) = 1;
        return true;
    }

    void ApplyWeaponSkeleton(uintptr_t player)
    {
        if (!player) return;

        // applies player skeleton based on the weapon they are holding. 1 = ellie, 2 = abby
        struct WeaponSkeleton { StringId64 weaponSid; uint8_t skeleton; };
        static const WeaponSkeleton kTable[] = {
            // primaries
            { SID("rifle-vepr"),                 2 },  // Abby
            { SID("rifle-galil"),                2 },  // Abby
            { SID("rifle-remington-bolt"),       1 },  // Ellie
            { SID("rifle-mpx5"),                 1 },  // Ellie
            { SID("shotgun-remington-pump"),     1 },  // Ellie
            { SID("shotgun-baserri-over-under"), 2 },  // Abby
            { SID("bow-ellie"),                  1 },  // Ellie
            { SID("crossbow"),                   2 },  // Abby
            // secondaries
            { SID("pistol-beretta"),             1 },  // Ellie
            { SID("pistol-glock"),               2 },  // Abby
            { SID("pistol-revolver-taurus"),     1 },  // Ellie
            { SID("pistol-xcaliber"),            2 },  // Abby
        };

        StringId64 weapon = GetPlayerWeaponId(player);

        uint8_t skeleton = 0;   // default rig unless the held weapon overrides it
        for (const auto& e : kTable) {
            if (e.weaponSid == weapon) { skeleton = e.skeleton; break; }
        }

        *reinterpret_cast<uint8_t*>(player + 0x608) = skeleton; // NOTE: skel override hooks will probably interfere
    }

    StringId64 GetMeleeAction(uintptr_t playerPtr)
    {
        if (playerPtr == 0) return 0;
        uintptr_t vtable = *reinterpret_cast<uintptr_t*>(playerPtr);
        auto getMeleeAction = reinterpret_cast<uintptr_t(__fastcall*)(uintptr_t)>(
            *reinterpret_cast<uintptr_t*>(vtable + 3728));
        uintptr_t result = getMeleeAction(playerPtr);
        if (!result) return 0;

        auto getSymbol = GameFn<uintptr_t(__fastcall*)(uintptr_t)>(0xEDE220);
        return *reinterpret_cast<uint64_t*>(getSymbol(result));
    }

    bool FireAttack(uintptr_t target, uintptr_t attacker, uint64_t weapon, uint64_t ammo, float damage, bool headshot)
    {
        if (!target || !attacker) return false;

        auto ctor = (uintptr_t(__fastcall*)(uintptr_t, int))(g_base + 0x394C10);
        auto fire = (uint8_t(__fastcall*)(uintptr_t, uintptr_t))(g_base + 0x395360);

        uint8_t* ai = (uint8_t*)calloc(1, 2700);
        ctor((uintptr_t)ai, 0);  // 0 = bullet

        *(uintptr_t*)(ai + 8) = *(uintptr_t*)(attacker + 128);  // attacker handle
        *(uint32_t*)(ai + 16) = *(uint32_t*)(attacker + 148);  // attacker instance
        *(uint64_t*)(ai + 32) = weapon;
        *(uint64_t*)(ai + 40) = ammo;
        *(float*)(ai + 64) = damage;
        memcpy(ai + 272, (void*)(target + 256), 80);
        memcpy(ai + 2272, (void*)(target + 256), 80);
        memcpy(ai + 2384, (void*)(attacker + 256), 80);
        *(uint32_t*)(ai + 2464) = 1;       // projectile type
        *(uint64_t*)(ai + 2472) = weapon;
        *(uint8_t*)(ai + 2484) = headshot ? 0x01 : 0x00;

        uint8_t result = 0;
        __try { result = fire((uintptr_t)ai, target); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        free(ai);
        return result != 0;
    }

    // The attack dispatch (0x395360) wraps the attack info in an "Attack" event and sends it to
    // the victim at once; it returns whether the victim's handler accepted it. FireAttack's
    // offsets (2272 / 2384 / 2464 / 2472, from Twelve Weeks) land on fields a real melee attack
    // uses in build 4188655 (damage copies, hit type, direction -- see the [melee-probe] dumps),
    // so its attacks are rejected. Variants (Shift+F6 cycles them, the log says which landed):
    //   1  Twelve Weeks' FireAttack layout (bullet)                     -- the old baseline
    //   2  bullet, only the fields a real attack fills: header, damage, target locator
    //   3  melee modelled on real local melee attacks (scalar fields only, no stack pointers)
    bool FireAttackVariant(int variant, uintptr_t target, uintptr_t identity, uintptr_t posSource, uint64_t weapon, float damage)
    {
        if (!target || !identity) return false;
        if (!posSource) posSource = identity;
        if (variant == 1) return FireAttack(target, identity, weapon, 0, damage);

        auto ctor = (uintptr_t(__fastcall*)(uintptr_t, int))(g_base + 0x394C10);
        auto fire = (uint8_t(__fastcall*)(uintptr_t, uintptr_t))(g_base + 0x395360);

        const int kind = variant == 3 ? 1 : 0;
        uint8_t* ai = (uint8_t*)calloc(1, 2800);
        ctor((uintptr_t)ai, kind);                                // attack kind at +0x18

        auto u32 = [&](size_t off, uint32_t v) { memcpy(ai + off, &v, 4); };
        auto f32 = [&](size_t off, float v) { memcpy(ai + off, &v, 4); };

        *(uintptr_t*)(ai + 0x08) = *(uintptr_t*)(identity + 128); // attacker handle
        u32(0x10, *(uint32_t*)(identity + 148));                  // attacker instance (process +0x94)
        *(uint64_t*)(ai + 0x20) = weapon;
        f32(0x40, damage);
        u32(0x44, 1);
        memcpy(ai + 0x110, (void*)(target + 256), 32);            // target locator (pos + rot)
        memcpy(ai + 0xA50, (void*)(target + 256), 32);            // second copy, as real attacks have
        *(uintptr_t*)(ai + 0x9D0) = target;                       // victim, as real attacks have
        *(uintptr_t*)(ai + 0x9F0) = target;
        *(uintptr_t*)(ai + 0xA80) = target;
        (void)posSource;
        if (kind == 1) {                                          // melee fields seen in real attacks
            u32(0x1C, 0xD);
            u32(0x34, 0xD);
            u32(0x38, 4);
            u32(0x3C, 1);
            f32(0x150, damage); u32(0x154, 0xD); u32(0x158, 2);
            ai[0x8A8] = 1;
            ai[0x8A9] = static_cast<uint8_t>((ai[0x8A9] & ~0x1B) | 0x04);
            f32(0x8B8, 13.0f); f32(0x8BC, 1.0f); f32(0x8C0, 0.1f);
            f32(0x960, damage);
            f32(0x970, damage); f32(0x974, 1.0f);
            f32(0x9A0, damage);
            f32(0x9B0, damage);
            u32(0xA00, 1);
            f32(0xA30, damage);
        }

        uint8_t result = 0;
        __try { result = fire((uintptr_t)ai, target); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        free(ai);
        return result != 0;
    }

    bool FireAttackEx(uintptr_t target, uintptr_t identity, uintptr_t posSource, uint64_t weapon, float damage, int kind)
    {
        return FireAttackVariant(kind == 1 ? 3 : 2, target, identity, posSource, weapon, damage);
    }

    // Fix for puppet weapon in hands

    float GetWeaponIkFade(uintptr_t player)
    {
        if (!player) return 0.0f;
        const uintptr_t ctrl = *reinterpret_cast<uintptr_t*>(player + 0x2FC8);
        if (!ctrl) return 0.0f;
        const uintptr_t vt = *reinterpret_cast<uintptr_t*>(ctrl);
        if (!vt) return 0.0f;
        const uintptr_t fn = *reinterpret_cast<uintptr_t*>(vt + 0x238);
        if (!fn) return 0.0f;

        const float f = reinterpret_cast<float(__fastcall*)(uintptr_t)>(fn)(ctrl);
        if (!(f > 0.0f)) return 0.0f;      // also rejects NaN
        return (f > 1.0f) ? 1.0f : f;
    }

    namespace {
        struct ProcessRef { uintptr_t handle; uint32_t instanceId; uint32_t pad; };

        using AttachFn = char(__fastcall*)(uintptr_t obj, const char* file, int line,
            const char* func, const ProcessRef* parent,
            StringId64 attachPoint, const float* offsetXform);

        StringId64 CurrentAttachPoint(uintptr_t obj)
        {
            if (!obj) return 0;
            static auto getAp = GameFn<uint64_t * (__fastcall*)(uintptr_t, uint64_t*)>(0x1CD4870);
            uint64_t sid = 0;
            getAp(obj, &sid);
            return sid;
        }
    }

    uintptr_t GetNpcWeapon(uintptr_t npc)
    {
        if (!npc) return 0;
        const uintptr_t vt = *reinterpret_cast<uintptr_t*>(npc);
        if (!vt) return 0;
        const uintptr_t fn = *reinterpret_cast<uintptr_t*>(vt + 0xD38);
        if (!fn) return 0;
        return reinterpret_cast<uintptr_t(__fastcall*)(uintptr_t)>(fn)(npc);
    }

    bool IsAimSideSwapped(uintptr_t player)
    {
        // Player::GunSide, DWORD at 0x4820. 1 = default, 0 = swapped.
        return player && *reinterpret_cast<const uint32_t*>(player + 0x4820) == 0;
    }

    bool AttachWeaponToCorrectHand(uintptr_t character, uint64_t weaponSid, bool mirrored)
    {
        if (!character || !weaponSid) return false;

        const uintptr_t weapon = GetNpcWeapon(character);
        if (!weapon) return false;

        static auto getArmIndex = GameFn<bool(__fastcall*)(uint64_t, uintptr_t)>(0x1161390);
        static auto weaponAnimType = GameFn<uint32_t(__fastcall*)(uintptr_t)>(0x1163A40);

        bool wantRight = getArmIndex(weaponSid, 0);

        if (mirrored && weaponAnimType(weapon) == 1)
            wantRight = !wantRight;

        const StringId64 want = wantRight ? SID("rightHand") : SID("leftHand");

        if (CurrentAttachPoint(weapon) == want) return true;

        const ProcessRef owner{
            *reinterpret_cast<uintptr_t*>(character + 0x80),
            *reinterpret_cast<uint32_t*>(character + 0x94), 0 };
        if (!owner.handle || !owner.instanceId) return false;

        alignas(16) static const float kIdentityXform[8] = { 0, 0, 0, 0, 0, 0, 0, 1.0f };

        static auto attach = GameFn<AttachFn>(0x1CD2140);
        const bool ok = attach(weapon, "", 0, "", &owner,
            want, kIdentityXform) != 0;
        if (!ok) {
            static uint32_t fails = 0;
            if ((fails++ % 120) == 0)
                TO1_LOG("[wpn] re-parent of %016llX to %s hand FAILED (%u)\n",
                    static_cast<unsigned long long>(weaponSid),
                    wantRight ? "right" : "left", fails);
        }
        return ok;
    }
}