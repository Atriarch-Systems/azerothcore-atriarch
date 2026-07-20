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
        std::string kind;   // travel | boss
    };

    char const* StatusName(Status s);

    // Route data (cached per map, loaded from mod_instance_route).
    std::vector<Waypoint> const& GetRoute(uint32 mapId);
    void ReloadRoutes();

    // Apply the correct per-instance raid/dungeon strategy set to a bot.
    // AiPlayerbot.ApplyInstanceStrategies does not fire when we teleport bots
    // in ourselves, so callers do it explicitly. Returns the strategy list the
    // bot ended up with (for logging/verification).
    std::string ApplyInstanceStrategies(Player* bot);

    // Start/stop/inspect a run. members = the controlled group.
    void StartRun(uint32 mapId, std::vector<ObjectGuid> const& members, uint32 startStep = 1);
    void PauseRun();
    void ResumeRun();
    void StopRun();
    void JumpToStep(uint32 step);

    bool IsActive();
    Status CurrentStatus();
    uint32 CurrentStep();
    uint32 RouteSize();
    uint32 RunMapId();
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
