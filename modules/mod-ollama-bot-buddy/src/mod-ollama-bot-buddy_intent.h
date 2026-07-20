#pragma once
#include "Player.h"
#include "ScriptMgr.h"
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Instance intent layer
//
// The playerbot AI supplies combat competence but no INTENT: dropped into a
// raid it fights whatever attacks it and otherwise stands still. This engine
// supplies the missing "go to the next objective" signal from authored route
// data (mod_instance_route), driving members along a route with navmesh
// pathing and pausing for combat.
//
// Deliberately independent of the raidlab command layer so phase 2 can attach
// the same machine to (a) a bot group-leader guiding a real player's group and
// (b) guild raid nights: construct an IntentRun with any set of player GUIDs.
// ---------------------------------------------------------------------------

namespace Intent
{
    enum class Status : uint8
    {
        Idle = 0,     // no run active
        Traveling,    // moving to the current waypoint
        Fighting,     // combat in progress; progression paused
        Waiting,      // at the waypoint, waiting for out-of-combat settle
        Done,         // route complete
        Wiped         // all members dead
    };

    struct Waypoint
    {
        uint32 step = 0;
        float x = 0.f, y = 0.f, z = 0.f;
        std::string label;
        std::string kind;      // travel | boss
        float radius = 0.f;    // 0 = use Intent.AdvanceRadius
        uint32 bossEntry = 0;  // creature entry for the kill test
        int32 bossIndex = -1;  // InstanceScript boss id, -1 = proximity test
    };

    char const* StatusName(Status s);

    // Route data. Keyed by route_key (world DB mod_guild_dungeon_route) so
    // several dungeons can share one map id (e.g. Scarlet Monastery wings).
    std::vector<Waypoint> const& GetRoute(std::string const& routeKey);
    void ReloadRoutes();

    // Apply the correct per-instance raid/dungeon strategy set to a bot.
    // PlayerbotAI::ApplyInstanceStrategies DOES also fire by itself on our
    // teleports, via HandleTeleportAck (PlayerbotAI.cpp:770-797) - an earlier
    // reading of ours claimed otherwise; the "no strategies" observation was a
    // dump taken BEFORE the ack arrived. The explicit call is kept because it
    // is idempotent (the function removes all instance strategies first) and
    // because it lets us log what actually attached. Callers should sample the
    // strategy list AFTER arrival, not immediately after issuing the teleport.
    std::string ApplyInstanceStrategies(Player* bot);

    // Start/stop/inspect a run. members = the controlled group.
    void StartRun(std::string const& routeKey, uint32 mapId, std::vector<ObjectGuid> const& members, uint32 startStep = 1);

    // Bind the run to a specific instance id. Members outside this instance are
    // ignored by the advance rule (they would otherwise block every step
    // forever with a cross-map distance) and are dropped from the active roster
    // after one re-teleport attempt.
    void SetInstance(uint32 instanceId, float ex, float ey, float ez, float eo);
    void PauseRun();
    void ResumeRun();
    void StopRun();
    void JumpToStep(uint32 step);

    bool IsActive();
    Status CurrentStatus();
    uint32 CurrentStep();
    uint32 RouteSize();
    uint32 RunMapId();
    std::string RunRouteKey();
    uint32 BossesDown();
    time_t StatusSince();
    Waypoint const* CurrentWaypoint();

    // Called from a world-thread tick.
    void Update();
}

// Config: Intent.Enable / AdvanceRadius / OutOfCombatSeconds / StuckTimeout
class IntentWorldScript : public WorldScript
{
public:
    IntentWorldScript();
    void OnStartup() override;
    void OnUpdate(uint32 diff) override;
};
