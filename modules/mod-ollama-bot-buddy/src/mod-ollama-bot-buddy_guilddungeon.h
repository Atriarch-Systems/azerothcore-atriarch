#pragma once
#include "Chat.h"
#include "ObjectGuid.h"
// Optional<> in the HandleStart signature — included explicitly rather than relying on
// Chat.h pulling it in transitively.
#include "Optional.h"
#include "ScriptMgr.h"
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Guild dungeon progression - Phase 1 (one guild, one dungeon, end to end).
//
// A guild's lowest-level eligible cohort forms a group, RALLIES at a town with
// a flight master, TRAVELS to the dungeon, ENTERS together (leader first so all
// members share one instance id), walks an authored route killing bosses,
// and HEARTHS home when the run ends.
//
// Catalog/route data lives in the WORLD db (mod_guild_dungeon_catalog /
// mod_guild_dungeon_route) so a character wipe cannot destroy hand-authored
// waypoints.
// ---------------------------------------------------------------------------

namespace GuildDungeon
{
    enum class Phase : uint8
    {
        Idle = 0,
        Rallying,       // gathering at the rally town
        Traveling,      // moving overland to the dungeon entrance
        AtMeetingStone, // checkpoint: fetch a non-cohort real player via the real meeting-stone
                        // summon mechanic before entering (docs/dungeon-leadership-and-summon.md,
                        // section 3b) - bounded by GuildDungeon.MeetingStoneWaitSeconds
        Entering,       // leader in first, then members (shared instance id)
        Running,        // intent layer drives the route
        Returning,      // hearthstone / teleport home
        Finished
    };

    char const* PhaseName(Phase p);

    struct Dungeon
    {
        uint32 dungeonId = 0;
        std::string routeKey;
        uint32 mapId = 0;
        uint8  difficulty = 0;
        std::string name;
        int8   team = 2;
        uint8  minLevel = 0;
        uint8  maxLevel = 0;
        uint32 entranceMap = 0;
        float  ex = 0, ey = 0, ez = 0, eo = 0;
        uint32 rallyMap = 0;
        float  rx = 0, ry = 0, rz = 0;
        std::string rallyName;
        uint8  groupSize = 5;
        bool   enabled = true;
    };

    struct Member
    {
        ObjectGuid guid;
        std::string name;
        uint8 level = 0;
        std::string role;
        std::string savedStrategies; // for exact restore on run end

        // --- Phase::Traveling flight sub-state ---
        // Persists which flightmaster (if any) this member is currently walking to / has
        // queued a multi-hop taxi route from, across ticks. Empty taxiNodes means "not
        // currently committed to a flight leg" - the tick handler resolves a fresh target
        // when it sees an empty list. See GuildDungeon.RealFlightMaster/MultiHopTaxi.
        uint32 flightMasterEntry = 0;   // creature template entry of the chosen flightmaster
        uint32 flightMasterMapId = 0;
        float  flightMasterX = 0, flightMasterY = 0, flightMasterZ = 0;
        std::vector<uint32> taxiNodes;  // multi-hop node list to activate on arrival
    };

    struct Run
    {
        bool active = false;
        uint32 guildId = 0;
        std::string guildName;
        Dungeon dungeon;
        std::vector<Member> members;
        ObjectGuid leader;
        uint32 groupId = 0;
        uint32 instanceId = 0;
        Phase phase = Phase::Idle;
        time_t startedAt = 0;
        time_t phaseAt = 0;
        uint32 bossesDown = 0;

        // --- Phase::AtMeetingStone bookkeeping ---
        // Per-run cooldown so a non-cohort real player who is out of range doesn't get a fresh
        // "X wants to summon you" popup every tick - mirrors the LFG-side latecomer action's
        // implicit cooldown (Ai/Base/Trigger/LfgTriggers.cpp's 30s checkInterval). 0 means "no
        // summon attempted yet this run".
        time_t lastMeetingStoneSummonAttempt = 0;
    };

    // Catalog access
    void ReloadCatalog();
    std::vector<Dungeon> const& Catalog();
    Dungeon const* FindDungeon(uint32 dungeonId);

    // Run control (Phase 1: one run at a time)
    bool StartRun(uint32 guildId, uint32 dungeonId, bool force, std::string& err);
    void AbortRun(char const* reason);
    bool IsRunActive();
    Run const& CurrentRun();

    // Managed-bot bookkeeping shared with mod-playerbots' ManagedBotRegistry
    bool IsManagedMember(ObjectGuid guid);

    void Update();

    // Explains, per guild, why no cohort could be formed (critique item 10).
    std::string ExplainCohorts();
}

class GuildDungeonWorldScript : public WorldScript
{
public:
    GuildDungeonWorldScript();
    void OnStartup() override;
    void OnUpdate(uint32 diff) override;
};

class GuildDungeonCommandScript : public CommandScript
{
public:
    GuildDungeonCommandScript();
    Acore::ChatCommands::ChatCommandTable GetCommands() const override;

    // dungeonId is Optional so `.guilddungeon start <guildId>` parses: as a plain uint32 the
    // command parser demanded both arguments, which made StartRun's "0 = first enabled
    // catalog entry" fallback unreachable from the command line.
    static bool HandleStart(ChatHandler* handler, uint32 guildId, Optional<uint32> dungeonId);
    static bool HandleForce(ChatHandler* handler, uint32 guildId);
    static bool HandleStatus(ChatHandler* handler);
    static bool HandleAbort(ChatHandler* handler);
    static bool HandleCohorts(ChatHandler* handler);
};
