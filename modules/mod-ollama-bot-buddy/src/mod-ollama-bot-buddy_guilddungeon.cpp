#include "mod-ollama-bot-buddy_guilddungeon.h"
#include "mod-ollama-bot-buddy_intent.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Log.h"
#include "ManagedBotRegistry.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "World.h"
#include <algorithm>
#include <cstdlib>
#include <sstream>

using namespace Acore::ChatCommands;

namespace
{
    // --- config ---
    bool   g_enable = true;
    uint32 g_tickSeconds = 30;
    uint32 g_minLevel = 15;
    uint32 g_levelBand = 6;
    uint32 g_minMembers = 5;
    uint32 g_maxRunMinutes = 45;
    uint32 g_wipeGraceSeconds = 60;
    uint32 g_lootGraceSeconds = 20;
    uint32 g_rallySeconds = 120;      // max time to wait for the group to gather
    uint32 g_travelSeconds = 600;     // max overland travel time before giving up and teleporting
    bool   g_debug = true;

    std::vector<GuildDungeon::Dungeon> g_catalog;
    bool g_catalogLoaded = false;

    GuildDungeon::Run g_run;

    // Rejection accounting for the "why no cohort" explain command.
    struct CohortReport
    {
        uint32 guildId = 0;
        std::string guildName;
        uint32 online = 0, eligible = 0, grouped = 0, tooLow = 0, dead = 0, inCombat = 0, notRandom = 0;
        uint8 anchorLevel = 0, widestBand = 0;
        std::string verdict;
    };
    std::vector<CohortReport> g_lastReports;

    void GdLog(char const* fmt) { LOG_INFO("module.guilddungeon", "{}", fmt); }

    template <typename... Args>
    void GdLogf(char const* fmt, Args&&... args)
    {
        LOG_INFO("module.guilddungeon", fmt, std::forward<Args>(args)...);
    }

    std::string JoinStrategies(PlayerbotAI* ai)
    {
        std::ostringstream oss;
        for (auto const& s : ai->GetStrategies(BOT_STATE_NON_COMBAT))
            oss << s << ",";
        return oss.str();
    }

    // Strategies that make a bot wander off on its own errands. Re-asserted
    // every tick because ChangeTalentsAction / PlayerbotFactory::Randomize
    // call ResetStrategies() mid-run and silently re-add "grind".
    char const* kSuppress = "-grind,-new rpg,-rpg,-move random,-lfg,-travel";

    void SuppressWandering(Player* bot)
    {
        if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot))
            ai->ChangeStrategy(kSuppress, BOT_STATE_NON_COMBAT);
    }

    Group* RunGroup()
    {
        if (Player* leader = ObjectAccessor::FindPlayer(g_run.leader))
            return leader->GetGroup();
        return nullptr;
    }

    std::vector<Player*> LiveMembers()
    {
        std::vector<Player*> out;
        for (auto const& m : g_run.members)
            if (Player* p = ObjectAccessor::FindPlayer(m.guid))
                if (p->IsInWorld())
                    out.push_back(p);
        return out;
    }

    void RegisterManaged(bool on)
    {
        for (auto const& m : g_run.members)
        {
            if (on) sManagedBotRegistry.AddBot(m.guid.GetCounter());
            else    sManagedBotRegistry.RemoveBot(m.guid.GetCounter());
        }
        if (g_run.groupId)
        {
            if (on) sManagedBotRegistry.AddGroup(g_run.groupId);
            else    sManagedBotRegistry.RemoveGroup(g_run.groupId);
        }
    }

    void SetPhase(GuildDungeon::Phase p)
    {
        g_run.phase = p;
        g_run.phaseAt = time(nullptr);
        GdLogf("PHASE {} guild='{}' dungeon='{}'", GuildDungeon::PhaseName(p), g_run.guildName, g_run.dungeon.name);
    }
}

namespace GuildDungeon
{
    char const* PhaseName(Phase p)
    {
        switch (p)
        {
            case Phase::Idle:      return "idle";
            case Phase::Rallying:  return "rallying";
            case Phase::Traveling: return "traveling";
            case Phase::Entering:  return "entering";
            case Phase::Running:   return "running";
            case Phase::Returning: return "returning";
            case Phase::Finished:  return "finished";
        }
        return "?";
    }

    void ReloadCatalog()
    {
        g_catalogLoaded = true;
        g_catalog.clear();
        if (QueryResult r = WorldDatabase.Query(
            "SELECT dungeon_id, route_key, map_id, difficulty, name, team, min_level, max_level, "
            "entrance_map, entrance_x, entrance_y, entrance_z, entrance_o, "
            "rally_map, rally_x, rally_y, rally_z, rally_name, group_size, enabled "
            "FROM mod_guild_dungeon_catalog WHERE enabled = 1 ORDER BY tier ASC, dungeon_id ASC"))
        {
            do
            {
                Field* f = r->Fetch();
                Dungeon d;
                d.dungeonId   = f[0].Get<uint32>();
                d.routeKey    = f[1].Get<std::string>();
                d.mapId       = f[2].Get<uint32>();
                d.difficulty  = f[3].Get<uint8>();
                d.name        = f[4].Get<std::string>();
                d.team        = f[5].Get<int8>();
                d.minLevel    = f[6].Get<uint8>();
                d.maxLevel    = f[7].Get<uint8>();
                d.entranceMap = f[8].Get<uint32>();
                d.ex = f[9].Get<float>();  d.ey = f[10].Get<float>();
                d.ez = f[11].Get<float>(); d.eo = f[12].Get<float>();
                d.rallyMap = f[13].Get<uint32>();
                d.rx = f[14].Get<float>(); d.ry = f[15].Get<float>(); d.rz = f[16].Get<float>();
                d.rallyName = f[17].Get<std::string>();
                d.groupSize = f[18].Get<uint8>();
                d.enabled   = f[19].Get<uint8>() != 0;
                g_catalog.push_back(d);
            } while (r->NextRow());
        }
        GdLogf("Catalog loaded: {} enabled dungeons.", uint32(g_catalog.size()));
    }

    std::vector<Dungeon> const& Catalog()
    {
        if (!g_catalogLoaded)
            ReloadCatalog();
        return g_catalog;
    }

    Dungeon const* FindDungeon(uint32 dungeonId)
    {
        for (auto const& d : Catalog())
            if (d.dungeonId == dungeonId)
                return &d;
        return nullptr;
    }

    bool IsManagedMember(ObjectGuid guid)
    {
        for (auto const& m : g_run.members)
            if (m.guid == guid)
                return true;
        return false;
    }

    bool IsRunActive() { return g_run.active; }
    Run const& CurrentRun() { return g_run; }

    // --- cohort selection -------------------------------------------------

    namespace
    {
        bool ClassCanTank(uint8 cls)
        {
            return cls == CLASS_WARRIOR || cls == CLASS_DRUID || cls == CLASS_PALADIN || cls == CLASS_DEATH_KNIGHT;
        }
        bool ClassCanHeal(uint8 cls)
        {
            return cls == CLASS_PRIEST || cls == CLASS_PALADIN || cls == CLASS_DRUID || cls == CLASS_SHAMAN;
        }

        // Roles: below 20 talents do not exist, so spec detection is garbage -
        // use class capability. Above 20 prefer the module's spec predicates.
        std::string RoleFor(Player* p, bool wantTank, bool wantHeal)
        {
            if (p->GetLevel() >= 20)
            {
                if (wantTank && PlayerbotAI::IsTank(p, true)) return "tank";
                if (wantHeal && PlayerbotAI::IsHeal(p, true)) return "healer";
            }
            if (wantTank && ClassCanTank(p->getClass())) return "tank";
            if (wantHeal && ClassCanHeal(p->getClass())) return "healer";
            return "dps";
        }

        // Collect online guild members via BroadcastWorker (m_members is protected).
        struct RosterCollector
        {
            std::vector<Player*>* out;
            void operator()(Player* p) const { if (p && p->IsInWorld()) out->push_back(p); }
        };

        std::vector<Player*> GuildRoster(Guild* guild)
        {
            std::vector<Player*> members;
            RosterCollector collector{ &members };
            guild->BroadcastWorker(collector, nullptr);
            return members;
        }

        // Build the lowest-level eligible cohort for a guild. Returns empty on
        // failure and fills `report` with the reason.
        std::vector<Player*> BuildCohort(Guild* guild, bool force, CohortReport& report)
        {
            report.guildId = guild->GetId();
            report.guildName = guild->GetName();

            std::vector<Player*> roster = GuildRoster(guild);
            report.online = uint32(roster.size());

            std::vector<Player*> candidates;
            for (Player* p : roster)
            {
                if (!sPlayerbotsMgr.GetPlayerbotAI(p))         { ++report.notRandom; continue; }
                if (!sRandomPlayerbotMgr.IsRandomBot(p))       { ++report.notRandom; continue; }
                if (p->GetGroup())                             { ++report.grouped;   continue; }
                if (p->isDead())                               { ++report.dead;      continue; }
                if (p->IsInCombat())                           { ++report.inCombat;  continue; }
                if (!force && p->GetLevel() < g_minLevel)      { ++report.tooLow;    continue; }
                candidates.push_back(p);
            }
            report.eligible = uint32(candidates.size());

            if (candidates.size() < g_minMembers)
            {
                report.verdict = "too few eligible (" + std::to_string(candidates.size()) + "/" +
                                 std::to_string(g_minMembers) + ")";
                return {};
            }

            // Anchor on the LOWEST level candidate, then fill upward within the band:
            // "the group forms at the level of the lowest member".
            std::sort(candidates.begin(), candidates.end(),
                      [](Player* a, Player* b) { return a->GetLevel() < b->GetLevel(); });
            report.anchorLevel = candidates.front()->GetLevel();
            report.widestBand = uint8(candidates.back()->GetLevel() - candidates.front()->GetLevel());

            std::vector<Player*> band;
            uint8 anchor = candidates.front()->GetLevel();
            for (Player* p : candidates)
                if (force || p->GetLevel() <= anchor + g_levelBand)
                    band.push_back(p);

            if (band.size() < g_minMembers)
            {
                report.verdict = "band too narrow: only " + std::to_string(band.size()) +
                                 " within " + std::to_string(g_levelBand) + " levels of anchor " +
                                 std::to_string(uint32(anchor));
                return {};
            }

            // Compose 1 tank / 1 healer / 3 dps, degrading gracefully.
            std::vector<Player*> picked;
            Player* tank = nullptr;
            Player* healer = nullptr;
            for (Player* p : band)
            {
                if (!tank && RoleFor(p, true, false) == "tank") { tank = p; continue; }
                if (!healer && RoleFor(p, false, true) == "healer") { healer = p; continue; }
            }
            if (tank)   picked.push_back(tank);
            if (healer) picked.push_back(healer);
            for (Player* p : band)
            {
                if (picked.size() >= g_minMembers) break;
                if (p == tank || p == healer) continue;
                picked.push_back(p);
            }

            if (picked.size() < g_minMembers)
            {
                report.verdict = "composition failed (" + std::to_string(picked.size()) + ")";
                return {};
            }

            report.verdict = "ok";
            return picked;
        }
    }

    std::string ExplainCohorts()
    {
        std::ostringstream oss;
        for (auto const& r : g_lastReports)
        {
            oss << "guild " << r.guildId << " '" << r.guildName << "': " << r.online << " online, "
                << r.eligible << " eligible (grouped " << r.grouped << ", below L" << g_minLevel
                << " " << r.tooLow << ", dead " << r.dead << ", in combat " << r.inCombat
                << ", not random " << r.notRandom << ")";
            if (r.anchorLevel)
                oss << ", anchor L" << uint32(r.anchorLevel) << ", spread " << uint32(r.widestBand);
            oss << " -> " << r.verdict << "\n";
        }
        if (g_lastReports.empty())
            oss << "(no cohort evaluation has run yet - wait one tick or use .guilddungeon cohorts after enabling)\n";
        return oss.str();
    }

    // --- run lifecycle ----------------------------------------------------

    bool StartRun(uint32 guildId, uint32 dungeonId, bool force, std::string& err)
    {
        if (g_run.active)
        {
            err = "a run is already active (Phase 1 allows one)";
            return false;
        }

        Guild* guild = sGuildMgr->GetGuildById(guildId);
        if (!guild)
        {
            err = "no guild with id " + std::to_string(guildId);
            return false;
        }

        Dungeon const* dungeon = nullptr;
        if (dungeonId)
            dungeon = FindDungeon(dungeonId);
        else if (!Catalog().empty())
            dungeon = &Catalog().front();
        if (!dungeon)
        {
            err = "no enabled catalog dungeon" + (dungeonId ? (" with id " + std::to_string(dungeonId)) : std::string());
            return false;
        }

        if (Intent::GetRoute(dungeon->routeKey).empty())
        {
            err = "no route rows for route_key '" + dungeon->routeKey + "'";
            return false;
        }

        CohortReport report;
        std::vector<Player*> cohort = BuildCohort(guild, force, report);
        g_lastReports.clear();
        g_lastReports.push_back(report);
        if (cohort.empty())
        {
            err = "no cohort: " + report.verdict;
            return false;
        }

        g_run = Run{};
        g_run.active = true;
        g_run.guildId = guildId;
        g_run.guildName = guild->GetName();
        g_run.dungeon = *dungeon;
        g_run.startedAt = time(nullptr);
        g_run.leader = cohort.front()->GetGUID();

        std::ostringstream roster;
        for (Player* p : cohort)
        {
            Member m;
            m.guid = p->GetGUID();
            m.name = p->GetName();
            m.level = p->GetLevel();
            m.role = RoleFor(p, true, true);
            if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(p))
                m.savedStrategies = JoinStrategies(ai);
            g_run.members.push_back(m);
            roster << m.name << "(" << uint32(m.level) << " " << m.role << ") ";
        }

        // Form the group directly (world-thread context: WorldScript::OnUpdate
        // and command handlers both run there). Group::Create already sets
        // GROUP_LOOT + ITEM_QUALITY_UNCOMMON, so we do not touch loot settings.
        Player* leader = ObjectAccessor::FindPlayer(g_run.leader);
        if (!leader)
        {
            g_run = Run{};
            err = "leader vanished during formation";
            return false;
        }
        if (Group* old = leader->GetGroup())
            old->Disband();

        Group* group = new Group();
        if (!group->Create(leader))
        {
            delete group;
            g_run = Run{};
            err = "Group::Create failed";
            return false;
        }
        sGroupMgr->AddGroup(group);
        g_run.groupId = group->GetGUID().GetCounter();

        for (auto const& m : g_run.members)
        {
            if (m.guid == g_run.leader)
                continue;
            Player* p = ObjectAccessor::FindPlayer(m.guid);
            if (!p)
                continue;
            if (Group* g = p->GetGroup())
                g->RemoveMember(p->GetGUID());
            if (!group->AddMember(p))
                GdLogf("AddMember failed for '{}'", m.name);
        }

        RegisterManaged(true);
        for (Player* p : LiveMembers())
            SuppressWandering(p);

        GdLogf("FORM guild='{}' ({}) dungeon='{}' members=[{}]", g_run.guildName, guildId, g_run.dungeon.name, roster.str());
        SetPhase(g_run.dungeon.rallyMap || g_run.dungeon.rx != 0.f ? Phase::Rallying : Phase::Entering);
        return true;
    }

    void AbortRun(char const* reason)
    {
        if (!g_run.active)
            return;
        GdLogf("EXIT result=abort reason='{}' duration={}s bosses={}", reason,
               uint32(time(nullptr) - g_run.startedAt), g_run.bossesDown);

        Intent::StopRun();

        // Restore strategies exactly, send everyone home, disband, unregister.
        for (auto const& m : g_run.members)
        {
            Player* p = ObjectAccessor::FindPlayer(m.guid);
            if (!p)
                continue;
            if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(p))
                if (!m.savedStrategies.empty())
                    ai->ChangeStrategy("+grind,+rpg", BOT_STATE_NON_COMBAT);
            p->CombatStop(true);
            if (!p->IsAlive())
                p->ResurrectPlayer(1.0f);
            // Hearthstone flavor: TeleportToEntryPoint() returns the bot to where
            // it entered from, falling back to its homebind (Player.cpp:1605-1611) -
            // exactly the "hearth out" behavior, using the core's own path.
            if (!p->TeleportToEntryPoint())
                GdLogf("HEARTH failed for '{}' (left in place)", m.name);
        }

        if (Group* g = RunGroup())
            g->Disband();

        RegisterManaged(false);
        g_run = Run{};
    }

    void Update()
    {
        if (!g_enable || !g_run.active)
            return;

        time_t now = time(nullptr);
        std::vector<Player*> members = LiveMembers();

        // Re-assert the wandering suppression every tick: ChangeTalentsAction
        // and PlayerbotFactory::Randomize re-add "grind" behind our back.
        for (Player* p : members)
            SuppressWandering(p);

        // Membership loss (bot logged out / left) - a run with too few members
        // must not silently "complete".
        if (members.size() < g_minMembers && g_run.phase != Phase::Returning && g_run.phase != Phase::Finished)
        {
            GdLogf("MEMBER_LOSS {} of {} remain", uint32(members.size()), uint32(g_run.members.size()));
            AbortRun("membership loss");
            return;
        }

        if (now - g_run.startedAt > time_t(g_maxRunMinutes) * 60)
        {
            AbortRun("max run time");
            return;
        }

        Dungeon const& d = g_run.dungeon;

        switch (g_run.phase)
        {
            case Phase::Rallying:
            {
                // Gather at a town with a flight master near the guild's centre
                // of mass before setting out together.
                uint32 atRally = 0;
                for (Player* p : members)
                {
                    if (p->GetMapId() == d.rallyMap && p->GetDistance(d.rx, d.ry, d.rz) <= 30.0f)
                        ++atRally;
                    else if (p->GetMapId() != d.rallyMap)
                        p->TeleportTo(d.rallyMap, d.rx, d.ry, d.rz, 0.0f); // cross-continent: teleport to the rally town
                    else
                        p->GetMotionMaster()->MovePoint(0, d.rx, d.ry, d.rz, FORCED_MOVEMENT_NONE, 0.f, 0.f, true, false);
                }
                if (atRally >= members.size() || (now - g_run.phaseAt) > time_t(g_rallySeconds))
                {
                    GdLogf("RALLY complete at '{}' ({}/{} gathered)", d.rallyName, atRally, uint32(members.size()));
                    SetPhase(Phase::Traveling);
                }
                break;
            }

            case Phase::Traveling:
            {
                // Overland travel to the instance portal. Ground movement only in
                // this pass - flight-path integration is a TODO (see report).
                uint32 arrived = 0;
                for (Player* p : members)
                {
                    if (p->GetMapId() == d.entranceMap && p->GetDistance(d.ex, d.ey, d.ez) <= 40.0f)
                        ++arrived;
                    else if (p->GetMapId() == d.entranceMap)
                        p->GetMotionMaster()->MovePoint(0, d.ex, d.ey, d.ez, FORCED_MOVEMENT_NONE, 0.f, 0.f, true, false);
                }
                if (arrived >= members.size())
                {
                    GdLog("TRAVEL complete on foot");
                    SetPhase(Phase::Entering);
                }
                else if ((now - g_run.phaseAt) > time_t(g_travelSeconds))
                {
                    // TODO(flight paths): reuse TaxiAction so the group flies
                    // between continents/zones instead of this fallback.
                    GdLogf("TRAVEL timeout after {}s ({} of {} walked there) - teleporting the rest to the portal. "
                           "TODO: flight-path travel not yet implemented.",
                           g_travelSeconds, arrived, uint32(members.size()));
                    SetPhase(Phase::Entering);
                }
                break;
            }

            case Phase::Entering:
            {
                // Leader enters FIRST and we wait for the instance bind, so
                // every member lands in the same instance id. Teleporting all
                // five at once races InstanceMap::AddPlayerToMap and can create
                // five separate instances.
                Player* leader = ObjectAccessor::FindPlayer(g_run.leader);
                if (!leader)
                {
                    AbortRun("leader lost during entry");
                    return;
                }

                if (leader->GetMapId() != d.mapId)
                {
                    if (!leader->TeleportTo(d.mapId, d.ex, d.ey, d.ez, d.eo))
                        GdLogf("ENTER leader TeleportTo({}) REFUSED for '{}' - instance limit "
                               "(AccountInstancesPerHour), bind mismatch or level gate.", d.mapId, leader->GetName());
                    return; // wait for the ack
                }

                if (leader->GetInstanceId() == 0)
                    return; // bind not established yet

                if (!g_run.instanceId)
                {
                    g_run.instanceId = leader->GetInstanceId();
                    GdLogf("ENTER map={} inst={} bot='{}' (leader)", d.mapId, g_run.instanceId, leader->GetName());
                }

                uint32 inside = 1;
                for (Player* p : members)
                {
                    if (p == leader)
                        continue;
                    if (p->GetMapId() == d.mapId)
                    {
                        ++inside;
                        if (p->GetInstanceId() != g_run.instanceId)
                            GdLogf("ENTER MISMATCH bot='{}' inst={} expected={}", p->GetName(),
                                   p->GetInstanceId(), g_run.instanceId);
                        else
                            GdLogf("ENTER map={} inst={} bot='{}'", d.mapId, p->GetInstanceId(), p->GetName());
                        continue;
                    }
                    if (!p->TeleportTo(d.mapId, d.ex, d.ey, d.ez, d.eo))
                        GdLogf("ENTER TeleportTo({}) REFUSED for '{}' - instance limit, bind mismatch or level gate.",
                               d.mapId, p->GetName());
                }

                if (inside >= members.size())
                {
                    // Idempotent; HandleTeleportAck also applies these on arrival.
                    for (Player* p : members)
                        GdLogf("STRATEGIES bot='{}' combat=[{}]", p->GetName(), Intent::ApplyInstanceStrategies(p));

                    std::vector<ObjectGuid> guids;
                    for (Player* p : members)
                        guids.push_back(p->GetGUID());
                    Intent::StartRun(d.routeKey, d.mapId, guids, 1);
                    // Members outside this instance are ignored by the advance
                    // rule (and re-teleported once) rather than stalling it.
                    Intent::SetInstance(g_run.instanceId, d.ex, d.ey, d.ez, d.eo);
                    Intent::ResumeRun();
                    SetPhase(Phase::Running);
                }
                break;
            }

            case Phase::Running:
            {
                if (Intent::CurrentStatus() == Intent::Status::Done)
                {
                    GdLogf("RUN complete: {} steps, {} bosses", Intent::CurrentStep(), Intent::BossesDown());
                    g_run.bossesDown = Intent::BossesDown();
                    SetPhase(Phase::Returning);
                }
                else if (Intent::CurrentStatus() == Intent::Status::Wiped &&
                         (now - Intent::StatusSince()) > time_t(g_wipeGraceSeconds))
                {
                    GdLog("RUN wiped past grace period");
                    g_run.bossesDown = Intent::BossesDown();
                    SetPhase(Phase::Returning);
                }
                break;
            }

            case Phase::Returning:
            {
                // Loot grace: auto-equip is packet-driven and asynchronous.
                if ((now - g_run.phaseAt) < time_t(g_lootGraceSeconds))
                    break;
                GdLogf("EXIT result={} duration={}s bosses={}",
                       Intent::CurrentStatus() == Intent::Status::Wiped ? "wipe" : "complete",
                       uint32(now - g_run.startedAt), g_run.bossesDown);
                AbortRun("run finished");
                break;
            }

            default:
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// Scripts
// ---------------------------------------------------------------------------

GuildDungeonWorldScript::GuildDungeonWorldScript() : WorldScript("GuildDungeonWorldScript") {}

void GuildDungeonWorldScript::OnStartup()
{
    g_enable            = sConfigMgr->GetOption<bool>("GuildDungeon.Enable", true);
    g_tickSeconds       = sConfigMgr->GetOption<uint32>("GuildDungeon.TickSeconds", 30);
    g_minLevel          = sConfigMgr->GetOption<uint32>("GuildDungeon.MinLevel", 15);
    g_levelBand         = sConfigMgr->GetOption<uint32>("GuildDungeon.LevelBand", 6);
    g_minMembers        = sConfigMgr->GetOption<uint32>("GuildDungeon.MinMembers", 5);
    g_maxRunMinutes     = sConfigMgr->GetOption<uint32>("GuildDungeon.MaxRunMinutes", 45);
    g_wipeGraceSeconds  = sConfigMgr->GetOption<uint32>("GuildDungeon.WipeGraceSeconds", 60);
    g_lootGraceSeconds  = sConfigMgr->GetOption<uint32>("GuildDungeon.LootGraceSeconds", 20);
    g_rallySeconds      = sConfigMgr->GetOption<uint32>("GuildDungeon.RallySeconds", 120);
    g_travelSeconds     = sConfigMgr->GetOption<uint32>("GuildDungeon.TravelSeconds", 600);
    g_debug             = sConfigMgr->GetOption<bool>("GuildDungeon.Debug", true);

    LOG_INFO("module.guilddungeon",
        "[GuildDungeon] Enable={}, TickSeconds={}, MinLevel={}, LevelBand={}, MinMembers={}, MaxRunMinutes={}",
        g_enable ? "true" : "false", g_tickSeconds, g_minLevel, g_levelBand, g_minMembers, g_maxRunMinutes);

    GuildDungeon::ReloadCatalog();
}

void GuildDungeonWorldScript::OnUpdate(uint32 /*diff*/)
{
    if (!g_enable)
        return;

    static time_t nextTick = 0;
    time_t now = time(nullptr);

    // Active runs tick every second; cohort scanning uses the slow cadence.
    if (GuildDungeon::IsRunActive())
    {
        static time_t nextRunTick = 0;
        if (now >= nextRunTick)
        {
            nextRunTick = now + 1;
            GuildDungeon::Update();
        }
        return;
    }

    if (now < nextTick)
        return;
    nextTick = now + g_tickSeconds;
    // Phase 1 does not auto-start runs: use .guilddungeon start/force. The
    // scheduler arrives in Phase 2.
}

GuildDungeonCommandScript::GuildDungeonCommandScript() : CommandScript("GuildDungeonCommandScript") {}

ChatCommandTable GuildDungeonCommandScript::GetCommands() const
{
    static ChatCommandTable table =
    {
        { "start",   HandleStart,   SEC_ADMINISTRATOR, Console::Yes },
        { "force",   HandleForce,   SEC_ADMINISTRATOR, Console::Yes },
        { "status",  HandleStatus,  SEC_ADMINISTRATOR, Console::Yes },
        { "abort",   HandleAbort,   SEC_ADMINISTRATOR, Console::Yes },
        { "cohorts", HandleCohorts, SEC_ADMINISTRATOR, Console::Yes },
    };
    static ChatCommandTable commandTable =
    {
        { "guilddungeon", table }
    };
    return commandTable;
}

bool GuildDungeonCommandScript::HandleStart(ChatHandler* handler, uint32 guildId, uint32 dungeonId)
{
    std::string err;
    if (!GuildDungeon::StartRun(guildId, dungeonId, false, err))
    {
        handler->SendSysMessage(("guilddungeon start failed: " + err).c_str());
        return true;
    }
    handler->SendSysMessage("guilddungeon: run started (watch the module.guilddungeon log).");
    return true;
}

bool GuildDungeonCommandScript::HandleForce(ChatHandler* handler, uint32 guildId)
{
    // Smoke-test path: ignores MinLevel and the level band entirely.
    std::string err;
    if (!GuildDungeon::StartRun(guildId, 0, true, err))
    {
        handler->SendSysMessage(("guilddungeon force failed: " + err).c_str());
        return true;
    }
    handler->SendSysMessage("guilddungeon: FORCED run started (level gates bypassed).");
    return true;
}

bool GuildDungeonCommandScript::HandleStatus(ChatHandler* handler)
{
    if (!GuildDungeon::IsRunActive())
    {
        handler->SendSysMessage("guilddungeon: no active run.");
        std::ostringstream cat;
        cat << "catalog: " << GuildDungeon::Catalog().size() << " enabled dungeon(s)";
        for (auto const& d : GuildDungeon::Catalog())
            cat << " | " << d.dungeonId << ":" << d.name << " (L" << uint32(d.minLevel) << "-"
                << uint32(d.maxLevel) << ", route '" << d.routeKey << "')";
        handler->SendSysMessage(cat.str().c_str());
        return true;
    }

    GuildDungeon::Run const& r = GuildDungeon::CurrentRun();
    std::ostringstream head;
    head << "guilddungeon: guild '" << r.guildName << "' in '" << r.dungeon.name << "' | phase "
         << GuildDungeon::PhaseName(r.phase) << " | inst " << r.instanceId
         << " | intent " << Intent::StatusName(Intent::CurrentStatus()) << " step " << Intent::CurrentStep()
         << "/" << Intent::RouteSize() << " | elapsed " << uint32(time(nullptr) - r.startedAt) << "s";
    handler->SendSysMessage(head.str().c_str());

    for (auto const& m : r.members)
    {
        Player* p = ObjectAccessor::FindPlayer(m.guid);
        std::ostringstream line;
        line << "  " << m.name << " (" << uint32(m.level) << " " << m.role << ") ";
        if (!p) line << "OFFLINE";
        else
            line << (p->IsAlive() ? "alive" : "DEAD") << " hp " << uint32(p->GetHealthPct())
                 << "% map " << p->GetMapId() << " inst " << p->GetInstanceId()
                 << (p->IsInCombat() ? " in-combat" : "");
        handler->SendSysMessage(line.str().c_str());
    }
    return true;
}

bool GuildDungeonCommandScript::HandleAbort(ChatHandler* handler)
{
    if (!GuildDungeon::IsRunActive())
    {
        handler->SendSysMessage("guilddungeon: no active run.");
        return true;
    }
    GuildDungeon::AbortRun("GM abort");
    handler->SendSysMessage("guilddungeon: run aborted, members sent home.");
    return true;
}

bool GuildDungeonCommandScript::HandleCohorts(ChatHandler* handler)
{
    std::string explain = GuildDungeon::ExplainCohorts();
    std::istringstream iss(explain);
    std::string line;
    while (std::getline(iss, line))
        if (!line.empty())
            handler->SendSysMessage(line.c_str());
    return true;
}
