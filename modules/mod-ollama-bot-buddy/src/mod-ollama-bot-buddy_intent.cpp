#include "mod-ollama-bot-buddy_intent.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Map.h"
#include "Creature.h"
#include "CellImpl.h"
#include "GridNotifiersImpl.h"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <sstream>
#include <unordered_set>

namespace
{
    bool   g_intentEnable = true;
    float  g_advanceRadius = 15.0f;
    uint32 g_outOfCombatSeconds = 5;
    uint32 g_stuckTimeout = 120;

    std::map<std::string, std::vector<Intent::Waypoint>> g_routes;
    bool g_routesLoaded = false;

    struct Run
    {
        bool active = false;
        bool paused = false;
        std::string routeKey;
        uint32 mapId = 0;
        uint32 bossesDown = 0;
        time_t statusSince = 0;
        uint32 instanceId = 0;                       // 0 = do not filter by instance
        float ex = 0.f, ey = 0.f, ez = 0.f, eo = 0.f; // entrance, for re-teleport attempts
        std::unordered_map<uint64, uint8> reteleportTries;
        std::unordered_set<uint64> dropped;           // absent members, excluded from advance
        std::unordered_set<uint64> strategySampled;   // logged post-arrival strategy list once
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
        // World DB (reference data survives a character wipe).
        if (QueryResult r = WorldDatabase.Query(
            "SELECT route_key, step, x, y, z, label, kind, radius, boss_entry, boss_index "
            "FROM mod_guild_dungeon_route ORDER BY route_key ASC, step ASC"))
        {
            do
            {
                Field* f = r->Fetch();
                Intent::Waypoint wp;
                std::string key = f[0].Get<std::string>();
                wp.step      = f[1].Get<uint32>();
                wp.x         = f[2].Get<float>();
                wp.y         = f[3].Get<float>();
                wp.z         = f[4].Get<float>();
                wp.label     = f[5].Get<std::string>();
                wp.kind      = f[6].Get<std::string>();
                wp.radius    = f[7].Get<float>();
                wp.bossEntry = f[8].Get<uint32>();
                wp.bossIndex = f[9].Get<int32>();
                g_routes[key].push_back(wp);
            } while (r->NextRow());
        }
        uint32 maps = uint32(g_routes.size()), steps = 0;
        for (auto const& [m, v] : g_routes)
            steps += uint32(v.size());
        LOG_INFO("module.guilddungeon", "[Intent] Loaded {} route steps across {} routes.", steps, maps);
    }

    // Is the boss for this step dead? Prefers the InstanceScript boss state
    // when the route row supplies a boss_index; otherwise "no living creature
    // with this entry within 80y".
    bool EncounterCleared(Player* anyMember, Intent::Waypoint const& wp)
    {
        if (!anyMember)
            return false;

        if (wp.bossIndex >= 0)
            if (Map* map = anyMember->GetMap())
                if (InstanceMap* im = map->ToInstanceMap())
                    if (InstanceScript* script = im->GetInstanceScript())
                        return script->GetBossState(uint32(wp.bossIndex)) == DONE;

        if (!wp.bossEntry)
            return true; // nothing to test against - treat as cleared

        // Proximity test around the waypoint itself.
        std::list<Creature*> found;
        anyMember->GetCreatureListWithEntryInGrid(found, wp.bossEntry, 80.0f);
        for (Creature* c : found)
            if (c && c->IsAlive())
                return false;
        return true;
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

    std::vector<Waypoint> const& GetRoute(std::string const& routeKey)
    {
        static std::vector<Waypoint> empty;
        if (!g_routesLoaded)
            LoadRoutes();
        auto it = g_routes.find(routeKey);
        return it == g_routes.end() ? empty : it->second;
    }

    void ReloadRoutes() { LoadRoutes(); }

    std::string ApplyInstanceStrategies(Player* bot)
    {
        PlayerbotAI* ai = bot ? sPlayerbotsMgr.GetPlayerbotAI(bot) : nullptr;
        if (!ai)
            return "";

        // PlayerbotAI::ApplyInstanceStrategies resolves mapId -> strategy name
        // and swaps it into both engines. It ALSO fires by itself on our
        // teleports via HandleTeleportAck; calling it here is idempotent (it
        // removes every instance strategy first) and gives us a truthful
        // strategy list to log once the bot has actually arrived.
        ai->ApplyInstanceStrategies(bot->GetMapId());

        std::ostringstream oss;
        for (auto const& s : ai->GetStrategies(BOT_STATE_COMBAT))
            oss << s << ",";
        return oss.str();
    }

    void StartRun(std::string const& routeKey, uint32 mapId, std::vector<ObjectGuid> const& members, uint32 startStep)
    {
        auto const& route = GetRoute(routeKey);
        if (route.empty())
        {
            LOG_INFO("module.guilddungeon", "[Intent] No route rows for key '{}' - nothing to drive.", routeKey);
            return;
        }

        g_run = Run{};
        g_run.active = true;
        g_run.routeKey = routeKey;
        g_run.mapId = mapId;
        g_run.statusSince = time(nullptr);
        g_run.step = std::max<uint32>(1, startStep);
        g_run.members = members;
        g_run.status = Status::Traveling;
        g_run.stepEnteredAt = time(nullptr);
        g_run.lastCombatAt = 0;

        LOG_INFO("module.guilddungeon", "[Intent] Run started on map {} with {} members, {} steps, starting at step {}.",
            mapId, uint32(members.size()), uint32(route.size()), g_run.step);
    }

    void SetInstance(uint32 instanceId, float ex, float ey, float ez, float eo)
    {
        g_run.instanceId = instanceId;
        g_run.ex = ex; g_run.ey = ey; g_run.ez = ez; g_run.eo = eo;
        LOG_INFO("module.guilddungeon", "[Intent] Run bound to instance {} (members outside it are ignored).", instanceId);
    }

    void PauseRun()  { g_run.paused = true;  LOG_INFO("module.guilddungeon", "[Intent] Run paused at step {}.", g_run.step); }
    void ResumeRun() { g_run.paused = false; g_run.stepEnteredAt = time(nullptr); g_run.stuckReported = false;
                       LOG_INFO("module.guilddungeon", "[Intent] Run resumed at step {}.", g_run.step); }
    void StopRun()   { LOG_INFO("module.guilddungeon", "[Intent] Run stopped at step {}.", g_run.step); g_run = Run{}; }

    void JumpToStep(uint32 step)
    {
        g_run.step = std::max<uint32>(1, step);
        g_run.status = Status::Traveling;
        g_run.stepEnteredAt = time(nullptr);
        g_run.lastMoveOrderAt = 0;
        g_run.stuckReported = false;
        LOG_INFO("module.guilddungeon", "[Intent] Jumped to step {}.", g_run.step);
    }

    bool IsActive()          { return g_run.active; }
    Status CurrentStatus()   { return g_run.active ? g_run.status : Status::Idle; }
    uint32 CurrentStep()     { return g_run.step; }
    uint32 RouteSize()       { return uint32(GetRoute(g_run.routeKey).size()); }
    uint32 BossesDown()      { return g_run.bossesDown; }
    time_t StatusSince()     { return g_run.statusSince; }
    uint32 RunMapId()        { return g_run.mapId; }
    std::string RunRouteKey(){ return g_run.routeKey; }

    Waypoint const* CurrentWaypoint()
    {
        auto const& route = GetRoute(g_run.routeKey);
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
            LOG_INFO("module.guilddungeon", "[Intent] Route complete on map {} (no step {}).", g_run.mapId, g_run.step);
            return;
        }

        std::vector<Player*> allMembers = LiveMembers();
        if (allMembers.empty())
            return;

        time_t now = time(nullptr);

        // Only members actually present in the run's map (and instance, when
        // bound) participate in the advance rule. A member who never entered
        // sits thousands of yards away and would otherwise block every step
        // forever - the exact failure seen live in Naxxramas.
        std::vector<Player*> members;   // present participants
        std::vector<Player*> outside;   // present in world, absent from the run
        for (Player* p : allMembers)
        {
            uint64 raw = p->GetGUID().GetRawValue();
            bool inMap = (p->GetMapId() == g_run.mapId);
            bool inInst = inMap && (g_run.instanceId == 0 || p->GetInstanceId() == g_run.instanceId);
            if (inInst)
            {
                // Recovered (or arrived late): welcome them back.
                if (g_run.dropped.erase(raw))
                    LOG_INFO("module.guilddungeon", "[Intent] '{}' rejoined the run (inst {}).",
                             p->GetName(), p->GetInstanceId());
                members.push_back(p);
                continue;
            }
            outside.push_back(p);
            if (g_run.dropped.count(raw))
                continue;

            uint8& tries = g_run.reteleportTries[raw];
            if (tries < 1 && g_run.ex != 0.f)
            {
                ++tries;
                bool ok = p->TeleportTo(g_run.mapId, g_run.ex, g_run.ey, g_run.ez, g_run.eo);
                LOG_INFO("module.guilddungeon", "[Intent] '{}' is outside the run (map {} inst {}) - re-teleport attempt {}.",
                         p->GetName(), p->GetMapId(), p->GetInstanceId(), ok ? "issued" : "REFUSED");
            }
            else
            {
                g_run.dropped.insert(raw);
                LOG_INFO("module.guilddungeon", "[Intent] DROPPED '{}' from the active roster: not in map {}"
                         " (is on map {} inst {}). Progression continues without them.",
                         p->GetName(), g_run.mapId, p->GetMapId(), p->GetInstanceId());
            }
        }

        if (members.empty())
            return; // nobody is actually in the instance yet

        uint32 alive = 0, inCombat = 0, atWaypoint = 0;
        for (Player* p : members)
        {
            if (!p->IsAlive())
                continue;
            ++alive;
            if (p->IsInCombat())
                ++inCombat;
            float radius = wp->radius > 0.f ? wp->radius : g_advanceRadius;
            if (p->GetDistance(wp->x, wp->y, wp->z) <= radius)
                ++atWaypoint;

            // Settle the "do raid strategies actually attach?" question with a
            // reading taken AFTER arrival (an immediately-post-teleport dump
            // samples the old map and always looks empty).
            uint64 raw = p->GetGUID().GetRawValue();
            if (!g_run.strategySampled.count(raw))
            {
                g_run.strategySampled.insert(raw);
                std::ostringstream st;
                for (auto const& s : sPlayerbotsMgr.GetPlayerbotAI(p)->GetStrategies(BOT_STATE_COMBAT))
                    st << s << ",";
                LOG_INFO("module.guilddungeon", "[Intent] STRATEGIES_ON_ARRIVAL bot='{}' map={} combat=[{}]",
                         p->GetName(), p->GetMapId(), st.str());
            }
        }

        if (alive == 0)
        {
            if (g_run.status != Status::Wiped)
            {
                g_run.status = Status::Wiped;
                g_run.statusSince = now;
                LOG_INFO("module.guilddungeon", "[Intent] WIPE at step {} ('{}') - progression stopped.", wp->step, wp->label);
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
                LOG_INFO("module.guilddungeon", "[Intent] Combat at step {} ('{}') - progression paused ({} in combat).",
                    wp->step, wp->label, inCombat);
            }
            return;
        }

        // Out-of-combat settle before advancing (loot/regen/stragglers).
        bool settled = (g_run.lastCombatAt == 0) || (now - g_run.lastCombatAt >= time_t(g_outOfCombatSeconds));

        // Hold while loot rolls are open, otherwise bots walk out of loot range
        // before Group::GroupLoot has enrolled everyone and the roll resolves.
        if (Player* any = members.front())
            if (Group* grp = any->GetGroup())
                if (grp->isRollLootActive())
                {
                    g_run.status = Status::Waiting;
                    return;
                }

        // A 'boss' step advances only when the encounter is actually cleared;
        // proximity alone would walk the group past a living boss.
        bool bossStep = (wp->kind == "boss");
        if (bossStep && atWaypoint >= alive && settled)
        {
            if (!EncounterCleared(members.front(), *wp))
            {
                g_run.status = Status::Waiting;
                return;
            }
            ++g_run.bossesDown;
            LOG_INFO("module.guilddungeon", "[Intent] BOSS_DOWN step={} label='{}' entry={}",
                     wp->step, wp->label, wp->bossEntry);
        }

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
            LOG_INFO("module.guilddungeon", "[Intent] Step {} ('{}', {}) complete - advancing to step {}.",
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
                if (p->GetDistance(wp->x, wp->y, wp->z) <= (wp->radius > 0.f ? wp->radius : g_advanceRadius))
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
            std::ostringstream ignored;
            for (Player* p : outside)
                ignored << p->GetName() << "(map " << p->GetMapId() << ") ";
            if (!outside.empty())
                detail << "| IGNORED (not in the instance, excluded from the advance rule): " << ignored.str();
            LOG_INFO("module.guilddungeon", "[Intent] stuck at step {} ('{}') for {}s - {}/{} at waypoint. Distances: {}",
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

    LOG_INFO("module.guilddungeon", "[Intent] Enable={}, AdvanceRadius={}, OutOfCombatSeconds={}, StuckTimeout={}",
        g_intentEnable ? "true" : "false", uint32(g_advanceRadius), g_outOfCombatSeconds, g_stuckTimeout);

    Intent::ReloadRoutes();
}

void IntentWorldScript::OnUpdate(uint32 /*diff*/)
{
    if (!g_intentEnable)
        return;

    // Cheap throttle: the engine only needs a second-scale cadence.
    static time_t nextTick = 0;
    time_t now = time(nullptr);
    if (now < nextTick)
        return;
    nextTick = now + 1;

    Intent::Update();
}
