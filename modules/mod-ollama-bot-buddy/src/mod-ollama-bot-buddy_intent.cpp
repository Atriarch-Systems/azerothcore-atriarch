#include "mod-ollama-bot-buddy_intent.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <sstream>

namespace
{
    bool   g_intentEnable = true;
    float  g_advanceRadius = 15.0f;
    uint32 g_outOfCombatSeconds = 5;
    uint32 g_stuckTimeout = 120;

    std::map<uint32, std::vector<Intent::Waypoint>> g_routes;
    bool g_routesLoaded = false;

    struct Run
    {
        bool active = false;
        bool paused = false;
        uint32 mapId = 0;
        uint32 step = 1;
        Intent::Status status = Intent::Status::Idle;
        std::vector<ObjectGuid> members;
        time_t stepEnteredAt = 0;
        time_t lastCombatAt = 0;
        time_t lastMoveOrderAt = 0;
        bool stuckReported = false;
    };
    Run g_run;

    void LoadRoutes()
    {
        g_routesLoaded = true;
        g_routes.clear();
        if (QueryResult r = CharacterDatabase.Query(
            "SELECT map_id, step, x, y, z, label, kind FROM mod_instance_route ORDER BY map_id ASC, step ASC"))
        {
            do
            {
                Field* f = r->Fetch();
                Intent::Waypoint wp;
                uint32 mapId = f[0].Get<uint32>();
                wp.step  = f[1].Get<uint32>();
                wp.x     = f[2].Get<float>();
                wp.y     = f[3].Get<float>();
                wp.z     = f[4].Get<float>();
                wp.label = f[5].Get<std::string>();
                wp.kind  = f[6].Get<std::string>();
                g_routes[mapId].push_back(wp);
            } while (r->NextRow());
        }
        uint32 maps = uint32(g_routes.size()), steps = 0;
        for (auto const& [m, v] : g_routes)
            steps += uint32(v.size());
        LOG_INFO("server.loading", "[Intent] Loaded {} route steps across {} maps.", steps, maps);
    }

    std::vector<Player*> LiveMembers()
    {
        std::vector<Player*> out;
        for (ObjectGuid const& g : g_run.members)
            if (Player* p = ObjectAccessor::FindPlayer(g))
                if (p->IsInWorld())
                    out.push_back(p);
        return out;
    }
}

namespace Intent
{
    char const* StatusName(Status s)
    {
        switch (s)
        {
            case Status::Idle:      return "idle";
            case Status::Traveling: return "traveling";
            case Status::Fighting:  return "fighting";
            case Status::Waiting:   return "waiting";
            case Status::Done:      return "done";
            case Status::Wiped:     return "wiped";
        }
        return "?";
    }

    std::vector<Waypoint> const& GetRoute(uint32 mapId)
    {
        static std::vector<Waypoint> empty;
        if (!g_routesLoaded)
            LoadRoutes();
        auto it = g_routes.find(mapId);
        return it == g_routes.end() ? empty : it->second;
    }

    void ReloadRoutes() { LoadRoutes(); }

    std::string ApplyInstanceStrategies(Player* bot)
    {
        PlayerbotAI* ai = bot ? sPlayerbotsMgr.GetPlayerbotAI(bot) : nullptr;
        if (!ai)
            return "";

        // The module's own mapping (PlayerbotAI::ApplyInstanceStrategies)
        // resolves mapId -> strategy name and swaps the strategy in for both
        // engines. It is public, so we call it directly: it does NOT fire on
        // our path because bots are teleported in by us rather than following
        // a master through the portal.
        ai->ApplyInstanceStrategies(bot->GetMapId());

        std::ostringstream oss;
        for (auto const& s : ai->GetStrategies(BOT_STATE_COMBAT))
            oss << s << ",";
        return oss.str();
    }

    void StartRun(uint32 mapId, std::vector<ObjectGuid> const& members, uint32 startStep)
    {
        auto const& route = GetRoute(mapId);
        if (route.empty())
        {
            LOG_INFO("server.loading", "[Intent] No route rows for map {} - nothing to drive.", mapId);
            return;
        }

        g_run = Run{};
        g_run.active = true;
        g_run.mapId = mapId;
        g_run.step = std::max<uint32>(1, startStep);
        g_run.members = members;
        g_run.status = Status::Traveling;
        g_run.stepEnteredAt = time(nullptr);
        g_run.lastCombatAt = 0;

        LOG_INFO("server.loading", "[Intent] Run started on map {} with {} members, {} steps, starting at step {}.",
            mapId, uint32(members.size()), uint32(route.size()), g_run.step);
    }

    void PauseRun()  { g_run.paused = true;  LOG_INFO("server.loading", "[Intent] Run paused at step {}.", g_run.step); }
    void ResumeRun() { g_run.paused = false; g_run.stepEnteredAt = time(nullptr); g_run.stuckReported = false;
                       LOG_INFO("server.loading", "[Intent] Run resumed at step {}.", g_run.step); }
    void StopRun()   { LOG_INFO("server.loading", "[Intent] Run stopped at step {}.", g_run.step); g_run = Run{}; }

    void JumpToStep(uint32 step)
    {
        g_run.step = std::max<uint32>(1, step);
        g_run.status = Status::Traveling;
        g_run.stepEnteredAt = time(nullptr);
        g_run.lastMoveOrderAt = 0;
        g_run.stuckReported = false;
        LOG_INFO("server.loading", "[Intent] Jumped to step {}.", g_run.step);
    }

    bool IsActive()          { return g_run.active; }
    Status CurrentStatus()   { return g_run.active ? g_run.status : Status::Idle; }
    uint32 CurrentStep()     { return g_run.step; }
    uint32 RouteSize()       { return uint32(GetRoute(g_run.mapId).size()); }
    uint32 RunMapId()        { return g_run.mapId; }

    Waypoint const* CurrentWaypoint()
    {
        auto const& route = GetRoute(g_run.mapId);
        for (auto const& wp : route)
            if (wp.step == g_run.step)
                return &wp;
        return nullptr;
    }

    void Update()
    {
        if (!g_intentEnable || !g_run.active || g_run.paused)
            return;

        Waypoint const* wp = CurrentWaypoint();
        if (!wp)
        {
            g_run.status = Status::Done;
            g_run.active = false;
            LOG_INFO("server.loading", "[Intent] Route complete on map {} (no step {}).", g_run.mapId, g_run.step);
            return;
        }

        std::vector<Player*> members = LiveMembers();
        if (members.empty())
            return;

        time_t now = time(nullptr);
        uint32 alive = 0, inCombat = 0, atWaypoint = 0;

        for (Player* p : members)
        {
            if (!p->IsAlive())
                continue;
            ++alive;
            if (p->IsInCombat())
                ++inCombat;
            if (p->GetMapId() == g_run.mapId && p->GetDistance(wp->x, wp->y, wp->z) <= g_advanceRadius)
                ++atWaypoint;
        }

        if (alive == 0)
        {
            if (g_run.status != Status::Wiped)
            {
                g_run.status = Status::Wiped;
                LOG_INFO("server.loading", "[Intent] WIPE at step {} ('{}') - progression stopped.", wp->step, wp->label);
            }
            return;
        }

        // Combat pauses progression entirely: the playerbot combat AI is good,
        // it just needs to be left alone.
        if (inCombat > 0)
        {
            g_run.lastCombatAt = now;
            if (g_run.status != Status::Fighting)
            {
                g_run.status = Status::Fighting;
                LOG_INFO("server.loading", "[Intent] Combat at step {} ('{}') - progression paused ({} in combat).",
                    wp->step, wp->label, inCombat);
            }
            return;
        }

        // Out-of-combat settle before advancing (loot/regen/stragglers).
        bool settled = (g_run.lastCombatAt == 0) || (now - g_run.lastCombatAt >= time_t(g_outOfCombatSeconds));

        if (atWaypoint >= alive && settled)
        {
            uint32 finished = wp->step;
            std::string label = wp->label;
            std::string kind = wp->kind;
            ++g_run.step;
            g_run.status = Status::Traveling;
            g_run.stepEnteredAt = now;
            g_run.lastMoveOrderAt = 0;
            g_run.stuckReported = false;
            LOG_INFO("server.loading", "[Intent] Step {} ('{}', {}) complete - advancing to step {}.",
                finished, label, kind, g_run.step);
            return;
        }

        g_run.status = (atWaypoint >= alive) ? Status::Waiting : Status::Traveling;

        // Independent navmesh pathing to the shared waypoint (no follow chain).
        // Re-issued periodically so bots that finish/abort a path keep going.
        if (now - g_run.lastMoveOrderAt >= 3)
        {
            g_run.lastMoveOrderAt = now;
            for (Player* p : members)
            {
                if (!p->IsAlive() || p->GetMapId() != g_run.mapId)
                    continue;
                if (p->GetDistance(wp->x, wp->y, wp->z) <= g_advanceRadius)
                    continue;
                // generatePath = true -> navmesh route, not straight-line.
                p->GetMotionMaster()->MovePoint(0, wp->x, wp->y, wp->z, FORCED_MOVEMENT_NONE, 0.f, 0.f,
                                                /*generatePath*/ true, /*forceDestination*/ false);
            }
        }

        // Stuck diagnostics: says whether pathing or the advance rule is at fault.
        if (!g_run.stuckReported && (now - g_run.stepEnteredAt) >= time_t(g_stuckTimeout))
        {
            g_run.stuckReported = true;
            std::ostringstream detail;
            for (Player* p : members)
            {
                detail << p->GetName() << "="
                       << (p->IsAlive() ? uint32(p->GetDistance(wp->x, wp->y, wp->z)) : 9999)
                       << "yd" << (p->IsAlive() ? "" : "(dead)") << " ";
            }
            LOG_INFO("server.loading", "[Intent] stuck at step {} ('{}') for {}s - {}/{} at waypoint. Distances: {}",
                wp->step, wp->label, g_stuckTimeout, atWaypoint, alive, detail.str());
        }
    }
}

IntentWorldScript::IntentWorldScript() : WorldScript("IntentWorldScript") {}

void IntentWorldScript::OnStartup()
{
    g_intentEnable        = sConfigMgr->GetOption<bool>("Intent.Enable", true);
    g_advanceRadius       = float(sConfigMgr->GetOption<uint32>("Intent.AdvanceRadius", 15));
    g_outOfCombatSeconds  = sConfigMgr->GetOption<uint32>("Intent.OutOfCombatSeconds", 5);
    g_stuckTimeout        = sConfigMgr->GetOption<uint32>("Intent.StuckTimeout", 120);

    if (const char* v = std::getenv("AC_INTENT_ENABLE"))
        if (*v) g_intentEnable = (*v == '1' || *v == 't' || *v == 'T');

    LOG_INFO("server.loading", "[Intent] Enable={}, AdvanceRadius={}, OutOfCombatSeconds={}, StuckTimeout={}",
        g_intentEnable ? "true" : "false", uint32(g_advanceRadius), g_outOfCombatSeconds, g_stuckTimeout);

    Intent::ReloadRoutes();
}

void IntentWorldScript::OnUpdate(uint32 /*diff*/)
{
    if (!g_intentEnable)
        return;

    static uint32 accum = 0;
    accum += 1;
    // Cheap throttle: the engine only needs a second-scale cadence.
    static time_t nextTick = 0;
    time_t now = time(nullptr);
    if (now < nextTick)
        return;
    nextTick = now + 1;

    Intent::Update();
}
