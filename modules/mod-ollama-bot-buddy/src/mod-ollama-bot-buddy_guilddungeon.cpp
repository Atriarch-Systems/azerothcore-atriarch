#include "mod-ollama-bot-buddy_guilddungeon.h"
#include "mod-ollama-bot-buddy_intent.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Log.h"
#include "ManagedBotRegistry.h"
// Reuses the reusable, real-core-mechanic meeting-stone summon helper built for the LFG-side
// latecomer case (section 3a) rather than the unrelated SummonAction/UseMeetingStoneAction
// cheat-teleport code - see docs/dungeon-leadership-and-summon.md, section 3b.
#include "MeetingStoneSummonHelper.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
// sObjectMgr: taxi-node lookup and path/cost resolution for flight travel.
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "TravelMgr.h"
#include "TravelNode.h"
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

    // Phase::AtMeetingStone (docs/dungeon-leadership-and-summon.md, section 3b): bounded pause at
    // the meeting-stone checkpoint, end of Phase::Traveling, for a non-cohort real player grouped
    // with the run to be fetched before proceeding to Phase::Entering.
    uint32 g_meetingStoneWaitSeconds = 90;

    // A reasonable meeting-stone search radius around the arrival/landing point. The LFG-side
    // latecomer case (LfgLatecomerAction.cpp) uses 40y from the bot itself at the moment it's
    // already standing at the instance entrance. Here the anchor bot is only guaranteed to be
    // within Phase::Traveling's own 40y "arrived" radius of d.ex/d.ey/d.ez, so a meeting stone
    // placed near the portal could be up to ~40y further away from THIS bot than it would be from
    // an LFG-teleported one standing right on top of it. 60y covers that slack.
    constexpr float kMeetingStoneSearchRange = 60.0f;

    // Per-run cooldown between summon attempts for the same not-yet-arrived real player, so the
    // checkpoint doesn't spam a fresh "X wants to summon you" popup every tick - matches the
    // design doc's "don't re-attempt more than once every 30s".
    constexpr uint32 kMeetingStoneSummonCooldownSeconds = 30;

    // Realistic travel (docs/playerbot-realistic-travel.md, step 1). Both default on; either
    // can be flipped to 0 live to instantly revert to the pre-fix behavior if something
    // regresses, without touching the other flag.
    bool   g_realFlightMaster = true;      // walk to an actual flightmaster before flying, instead
                                            // of snapping onto the taxi path from wherever the bot stands
    bool   g_multiHopTaxi = true;          // use the module's multi-hop taxi graph instead of the
                                            // core's single direct-path lookup
    uint32 g_flightApproachSeconds = 180;  // extra time budget added on top of g_travelSeconds to
                                            // cover the walk-to-flightmaster leg

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
    //
    // During Phase::Rallying only, "-new rpg,-rpg" are deliberately left OUT of the
    // suppression list (docs/playerbot-realistic-travel.md, step 3): AiFactory::
    // AddDefaultNonCombatStrategies now wires "new rpg"/"rpg" onto managed-group members too
    // (gated by AiPlayerbot.ManagedGroupRpgStrategies), and StartRun() calls
    // SelectiveResetStrategies(BOT_STATE_NON_COMBAT) on each member right after registering it
    // with ManagedBotRegistry so that wiring actually takes effect for already-spawned bots -
    // otherwise this suppression list would silently strip it back off on the very next tick.
    // The tradeoff is accepted as documented risk, not solved here: a managed bot may
    // occasionally wander off toward a quest or grind spot via the new-rpg state machine's own
    // global probability weighting while rallying, with no per-action carve-out available to
    // prevent it. AiPlayerbot.ManagedGroupRpgStrategies = 0 removes the wiring at the source,
    // making the narrower list below equivalent to the full one again.
    char const* kSuppressFull     = "-grind,-new rpg,-rpg,-move random,-lfg,-travel";
    char const* kSuppressRallying = "-grind,-move random,-lfg,-travel";

    void SuppressWandering(Player* bot, GuildDungeon::Phase phase)
    {
        if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot))
            ai->ChangeStrategy(phase == GuildDungeon::Phase::Rallying ? kSuppressRallying : kSuppressFull,
                               BOT_STATE_NON_COMBAT);
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

    // Mutable lookup into g_run.members by guid - used by the Traveling-phase flight logic to
    // persist per-member flight sub-state (which flightmaster, which multi-hop route) across
    // ticks. LiveMembers() above returns Player* copies with no way back to this record.
    GuildDungeon::Member* FindMemberRecord(ObjectGuid guid)
    {
        for (auto& m : g_run.members)
            if (m.guid == guid)
                return &m;
        return nullptr;
    }

    // --- Phase::AtMeetingStone helpers (docs/dungeon-leadership-and-summon.md, section 3b) ---

    // Nearest live member standing on `mapId` to (x, y, z) - used to anchor the meeting-stone
    // search on a bot that is actually near the landing point, since
    // MeetingStoneSummonHelper::FindNearestRealMeetingStone scans relative to a WorldObject, not
    // raw coordinates. Returns nullptr if no live member is on that map yet.
    Player* NearestMemberTo(std::vector<Player*> const& members, uint32 mapId, float x, float y, float z)
    {
        Player* nearest = nullptr;
        float nearestDist = 0.f;
        for (Player* p : members)
        {
            if (p->GetMapId() != mapId)
                continue;
            float dist = p->GetDistance(x, y, z);
            if (!nearest || dist < nearestDist)
            {
                nearest = p;
                nearestDist = dist;
            }
        }
        return nearest;
    }

    // Finds a real (non-playerbot) member of `group` who is not within earshot - the engine's
    // actual CONFIG_LISTEN_RANGE_SAY (sWorld->getFloatConfig, WorldConfig.cpp:423 "ListenRange.Say",
    // default 40y), never a hardcoded literal - of `landmark` (the meeting stone). Mirrors
    // LfgLatecomerValue::Calculate() (mod-playerbots' Ai/Base/Value/LfgValues.cpp), adapted from
    // LFG's "different map/instance" post-teleport check to this checkpoint's own "out of Say
    // range of the meeting stone" condition.
    //
    // IMPORTANT, as of this writing: BuildCohort()/StartRun() above only ever populate this run's
    // Group with guild-roster PLAYERBOTS (BuildCohort filters every candidate on
    // sPlayerbotsMgr.GetPlayerbotAI(p) && sRandomPlayerbotMgr.IsRandomBot(p)), and nothing else in
    // this file ever adds anyone else to that Group afterward. So in the current system this will
    // always return nullptr in practice, making Phase::AtMeetingStone a safe no-op every run - see
    // this feature's final report for the full explanation of what would need to be true for a
    // real player to ever be detected here.
    Player* FindUngroupedRealPlayer(Group* group, WorldObject* landmark)
    {
        if (!group || !landmark)
            return nullptr;

        float listenRange = sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY);
        for (Group::MemberSlot const& slot : group->GetMemberSlots())
        {
            Player* member = ObjectAccessor::FindPlayer(slot.guid);
            if (!member || !member->IsInWorld())
                continue; // fully offline members can't be meeting-stone summoned anyway
            if (sPlayerbotsMgr.GetPlayerbotAI(member))
                continue; // playerbot, not a real player

            if (member->GetMapId() != landmark->GetMapId() || member->GetDistance(landmark) > listenRange)
                return member;
        }
        return nullptr;
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
            case Phase::Idle:           return "idle";
            case Phase::Rallying:       return "rallying";
            case Phase::Traveling:      return "traveling";
            case Phase::AtMeetingStone: return "meetingstone";
            case Phase::Entering:       return "entering";
            case Phase::Running:        return "running";
            case Phase::Returning:      return "returning";
            case Phase::Finished:       return "finished";
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

    // --- travel -----------------------------------------------------------

    // Result of one tick's worth of flight-leg progress for a single member.
    enum class FlightLegResult : uint8
    {
        None,     // no flight in progress; caller should fall back to the ground walk
        Walking,  // en route to a flightmaster on foot; leave the member alone this tick
        Flying    // taxi just activated (or already in the air)
    };

    // Sum the fare across every consecutive hop in a resolved node list. ActivateTaxiPathTo
    // only ever debits the first hop's cost up front (Player.cpp), but it refuses the whole
    // itinerary outright unless the player can afford the TOTAL - so the shortfall has to be
    // topped up against this sum, not just the first hop's cost.
    uint32 TotalTaxiFare(std::vector<uint32> const& nodes)
    {
        uint32 total = 0;
        for (size_t i = 1; i < nodes.size(); ++i)
        {
            uint32 path = 0, cost = 0;
            sObjectMgr->GetTaxiPath(nodes[i - 1], nodes[i], path, cost);
            total += cost;
        }
        return total;
    }

    // Resolve the node list for a src -> dst flight, preferring the module's own multi-hop
    // taxi graph (which finds routes needing a transfer) and falling back to the core's
    // single-hop lookup - either because GuildDungeon.MultiHopTaxi is off, or because the
    // graph itself has no cached route for this pair. Returns an empty vector if no route
    // exists by either method.
    std::vector<uint32> ResolveTaxiNodes(uint32 src, uint32 dst)
    {
        if (!src || !dst || src == dst)
            return {};

        if (g_multiHopTaxi)
        {
            std::vector<uint32> nodes = sTravelNodeMap.FindTaxiPath(src, dst);
            if (!nodes.empty())
                return nodes;
        }

        uint32 path = 0, cost = 0;
        sObjectMgr->GetTaxiPath(src, dst, path, cost);
        if (!path)
            return {};
        return { src, dst };
    }

    // Pre-fix behavior, kept as the GuildDungeon.RealFlightMaster=0 kill switch: snaps the bot
    // onto the taxi path via the scripted/no-NPC call from wherever it stands, instead of
    // walking to a flightmaster first. Still honors GuildDungeon.MultiHopTaxi independently -
    // that flag closes a separate shortcut (single-hop-only routing) and reverting 1a should
    // not silently revert 1b too.
    //
    // spellid is left at 1 deliberately: with spellid == 0 the core's CONFIG_INSTANT_TAXI
    // shortcut collapses the whole journey into a teleport, and the journey is the point.
    FlightLegResult TryStartFlightLegacy(Player* p, Dungeon const& d)
    {
        if (!p->IsAlive() || p->IsInCombat())
            return FlightLegResult::None;
        // ActivateTaxiPathTo refuses these outright; checking first keeps the log quiet.
        if (p->HasUnitState(UNIT_STATE_STUNNED) || p->HasUnitState(UNIT_STATE_ROOT))
            return FlightLegResult::None;

        uint32 team = uint32(p->GetTeamId());
        uint32 src = sObjectMgr->GetNearestTaxiNode(p->GetPositionX(), p->GetPositionY(),
                                                    p->GetPositionZ(), p->GetMapId(), team);
        uint32 dst = sObjectMgr->GetNearestTaxiNode(d.ex, d.ey, d.ez, d.entranceMap, team);

        std::vector<uint32> nodes = ResolveTaxiNodes(src, dst);
        if (nodes.empty())
            return FlightLegResult::None; // no route; caller falls back to walking

        uint32 cost = TotalTaxiFare(nodes);
        // The fare is charged even on a scripted call, and a level-15 bot is usually broke —
        // without this the flight silently fails and the group walks into the same death
        // that stalled the first run. Cover only the shortfall.
        if (p->GetMoney() < cost)
            p->ModifyMoney(int32(cost - p->GetMoney()));

        if (!p->ActivateTaxiPathTo(nodes, nullptr, 1))
            return FlightLegResult::None;

        GdLogf("TRAVEL '{}' flying scripted taxi {} node(s) {} -> {} (cost {})",
               p->GetName(), uint32(nodes.size()), src, dst, cost);
        return FlightLegResult::Flying;
    }

    // Advance one member's Phase::Traveling flight leg by one tick.
    //
    // Step 1 fix: the old behavior (TryStartFlightLegacy above) called
    // ActivateTaxiPathTo(nodes, nullptr, 1) from wherever the bot happened to be standing -
    // the npc-less "scripted call" branch skips the flightmaster-proximity check entirely, so
    // the bot was silently teleported onto the flight path rather than walking there. This
    // walks the member to an actual flightmaster NPC first (mirrors NewRpgTravelFlightAction,
    // Ai/World/Rpg/Action/NewRpgAction.cpp:459-501) and only uses the real "taximaster" branch
    // (npc != nullptr) once the member is actually standing at it. The chosen flightmaster and
    // resolved route are cached on the Member record so a walk in progress isn't re-targeted
    // or re-resolved every tick.
    FlightLegResult AdvanceFlightLeg(Player* p, GuildDungeon::Member& m, Dungeon const& d)
    {
        if (!g_realFlightMaster)
            return TryStartFlightLegacy(p, d);

        if (!p->IsAlive() || p->IsInCombat())
            return FlightLegResult::None;
        if (p->HasUnitState(UNIT_STATE_STUNNED) || p->HasUnitState(UNIT_STATE_ROOT))
            return FlightLegResult::None;

        if (m.taxiNodes.empty())
        {
            TravelMgr::FlightMasterInfo const* info = sTravelMgr.GetNearestFlightMasterInfo(p);
            if (!info)
                return FlightLegResult::None; // no cached flightmaster on this map at all

            uint32 team = uint32(p->GetTeamId());
            uint32 dst = sObjectMgr->GetNearestTaxiNode(d.ex, d.ey, d.ez, d.entranceMap, team);
            if (!dst || info->taxiNodeId == dst)
                return FlightLegResult::None; // already essentially at the destination node

            std::vector<uint32> nodes = ResolveTaxiNodes(info->taxiNodeId, dst);
            if (nodes.empty())
                return FlightLegResult::None; // no route, even via the multi-hop graph

            m.flightMasterEntry = info->templateEntry;
            m.flightMasterMapId = info->pos.GetMapId();
            m.flightMasterX = info->pos.GetPositionX();
            m.flightMasterY = info->pos.GetPositionY();
            m.flightMasterZ = info->pos.GetPositionZ();
            m.taxiNodes = std::move(nodes);
        }

        // The map changed under us (e.g. a GM teleport) - the cached flightmaster is stale.
        // Drop it and let the caller fall back to the ground walk toward the entrance instead
        // of chasing a target on a map the member is no longer on.
        if (p->GetMapId() != m.flightMasterMapId)
        {
            m.taxiNodes.clear();
            return FlightLegResult::None;
        }

        if (p->GetDistance(m.flightMasterX, m.flightMasterY, m.flightMasterZ) > INTERACTION_DISTANCE)
        {
            p->GetMotionMaster()->MovePoint(0, m.flightMasterX, m.flightMasterY, m.flightMasterZ,
                                             FORCED_MOVEMENT_NONE, 0.f, 0.f, true, false);
            return FlightLegResult::Walking;
        }

        Creature* flightMaster = p->FindNearestCreature(m.flightMasterEntry, INTERACTION_DISTANCE * 3);
        if (!flightMaster || !flightMaster->IsAlive())
        {
            // NPC not there (despawned, or the cached position doesn't line up) - give up this
            // leg rather than stalling the member here forever; the caller's ground walk to
            // the entrance (and, in the worst case, the Phase::Entering timeout teleport) takes
            // over instead of introducing a second infinite wait.
            m.taxiNodes.clear();
            return FlightLegResult::None;
        }

        if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(p))
            ai->RemoveShapeshift();
        if (p->IsMounted())
            p->Dismount();
        p->GetSession()->SendLearnNewTaxiNode(flightMaster);

        uint32 cost = TotalTaxiFare(m.taxiNodes);
        if (p->GetMoney() < cost)
            p->ModifyMoney(int32(cost - p->GetMoney()));

        std::vector<uint32> nodes = m.taxiNodes;
        if (!p->ActivateTaxiPathTo(nodes, flightMaster, 0))
        {
            GdLogf("TRAVEL '{}' ActivateTaxiPathTo at flightmaster {} failed - falling back to ground",
                   p->GetName(), m.flightMasterEntry);
            m.taxiNodes.clear();
            return FlightLegResult::None;
        }

        GdLogf("TRAVEL '{}' flying {} node(s) from flightmaster {} (cost {})",
               p->GetName(), uint32(nodes.size()), m.flightMasterEntry, cost);
        m.taxiNodes.clear();
        return FlightLegResult::Flying;
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
        {
            // Managed-group RPG strategies (AiPlayerbot.ManagedGroupRpgStrategies) only take
            // effect through AiFactory::AddDefaultNonCombatStrategies, which runs at
            // construction/ResetStrategies/SelectiveResetStrategies time - never on group-join.
            // Without this, a bot that was already spawned before this run started would keep
            // running whatever non-combat strategy set it had before, oblivious to the
            // ManagedBotRegistry registration that just happened above.
            if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(p))
                ai->SelectiveResetStrategies(BOT_STATE_NON_COMBAT);
            SuppressWandering(p, g_run.phase);
        }

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
            SuppressWandering(p, g_run.phase);

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
                // Travel to the instance portal: fly where a flight path exists, walk the
                // last stretch, and teleport only as a timeout backstop. Making the journey
                // is part of the spec, so the taxi is the intended path and not an
                // optimisation over walking.
                //
                // The first live run stalled here permanently with the level-15 anchor lying
                // dead on the road through Westfall. A corpse can neither walk nor board a
                // flight, so `arrived` could never reach the full party and the run sat in
                // this phase until it timed out. Reviving en route is therefore part of
                // making travel work at all, not a separate nicety: on a long overland trip
                // the lowest member of a level-banded group WILL occasionally die, and the
                // run should survive that.
                uint32 arrived = 0, flying = 0, walkingToFm = 0, revived = 0;
                for (Player* p : members)
                {
                    if (!p->IsAlive())
                    {
                        p->ResurrectPlayer(0.5f);
                        p->SpawnCorpseBones();
                        ++revived;
                        continue;
                    }

                    if (p->IsInFlight())
                    {
                        ++flying;
                        continue;
                    }

                    if (p->GetMapId() == d.entranceMap && p->GetDistance(d.ex, d.ey, d.ez) <= 40.0f)
                    {
                        ++arrived;
                        continue;
                    }

                    Member* m = FindMemberRecord(p->GetGUID());

                    // Only bother with a flight for genuinely long hops; short ones walk. A
                    // member already committed to a flightmaster (non-empty taxiNodes) keeps
                    // going regardless of current distance to the entrance, so a walk already
                    // in progress isn't abandoned partway just because it happened to bring
                    // the member within 300y of the destination coordinates.
                    bool wantsFlight = m && (!m->taxiNodes.empty() || p->GetMapId() != d.entranceMap ||
                                             p->GetDistance(d.ex, d.ey, d.ez) > 300.0f);
                    if (wantsFlight)
                    {
                        FlightLegResult result = AdvanceFlightLeg(p, *m, d);
                        if (result == FlightLegResult::Flying)
                        {
                            ++flying;
                            continue;
                        }
                        if (result == FlightLegResult::Walking)
                        {
                            ++walkingToFm;
                            continue;
                        }
                    }

                    if (p->GetMapId() == d.entranceMap)
                        p->GetMotionMaster()->MovePoint(0, d.ex, d.ey, d.ez, FORCED_MOVEMENT_NONE, 0.f, 0.f, true, false);
                }

                if (revived)
                    GdLogf("TRAVEL revived {} member(s) who died en route", revived);

                // RealFlightMaster adds a real walk-to-flightmaster leg on top of the flight
                // itself, so it gets its own time budget on top of the base TravelSeconds
                // rather than eating into it - see GuildDungeon.FlightApproachSeconds.
                uint32 travelBudget = g_travelSeconds + (g_realFlightMaster ? g_flightApproachSeconds : 0);
                if (arrived >= members.size())
                {
                    GdLog("TRAVEL complete");
                    SetPhase(Phase::AtMeetingStone);
                }
                else if ((now - g_run.phaseAt) > time_t(travelBudget))
                {
                    GdLogf("TRAVEL timeout after {}s ({} arrived, {} flying, {} walking to a flightmaster) - "
                           "teleporting the rest to the portal",
                           travelBudget, arrived, flying, walkingToFm);
                    SetPhase(Phase::AtMeetingStone);
                }
                break;
            }

            case Phase::AtMeetingStone:
            {
                // Real-player fetch checkpoint (docs/dungeon-leadership-and-summon.md, section
                // 3b) - the ONLY place this run ever pauses for a real player. Phase::Rallying
                // and Phase::Traveling above never wait on anyone but guild-cohort bots, and that
                // is unchanged by this phase.
                Group* group = RunGroup();
                Player* anchor = NearestMemberTo(members, d.entranceMap, d.ex, d.ey, d.ez);

                GameObject* meetingStone = anchor
                    ? MeetingStoneSummonHelper::FindNearestRealMeetingStone(anchor, kMeetingStoneSearchRange)
                    : nullptr;
                Player* latecomer = meetingStone ? FindUngroupedRealPlayer(group, meetingStone) : nullptr;

                bool timedOut = (now - g_run.phaseAt) > time_t(g_meetingStoneWaitSeconds);
                if (!latecomer || timedOut)
                {
                    if (latecomer && timedOut)
                        GdLogf("MEETINGSTONE timeout after {}s waiting for real player '{}' - proceeding to Entering",
                               g_meetingStoneWaitSeconds, latecomer->GetName());
                    SetPhase(Phase::Entering);
                    break;
                }

                // A real, non-cohort player is grouped with this run and out of earshot of the
                // stone - summon them in via the leader (or the nearest bot, if the leader isn't
                // itself near the stone), at most once per kMeetingStoneSummonCooldownSeconds so
                // the summon popup isn't re-sent every tick.
                if (now - g_run.lastMeetingStoneSummonAttempt >= time_t(kMeetingStoneSummonCooldownSeconds))
                {
                    Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID());
                    Player* summoner = (leader && leader->IsInWorld() && leader->IsAlive() &&
                                        leader->GetMapId() == meetingStone->GetMapId() &&
                                        leader->GetDistance(meetingStone) <= kMeetingStoneSearchRange)
                                           ? leader : anchor;

                    g_run.lastMeetingStoneSummonAttempt = now;
                    if (MeetingStoneSummonHelper::SummonPlayerViaMeetingStone(summoner, latecomer, meetingStone))
                        GdLogf("MEETINGSTONE '{}' summoned late real player '{}' via meeting stone",
                               summoner->GetName(), latecomer->GetName());
                    else
                        GdLogf("MEETINGSTONE summon attempt failed for real player '{}'", latecomer->GetName());
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

    g_realFlightMaster      = sConfigMgr->GetOption<bool>("GuildDungeon.RealFlightMaster", true);
    g_multiHopTaxi          = sConfigMgr->GetOption<bool>("GuildDungeon.MultiHopTaxi", true);
    g_flightApproachSeconds = sConfigMgr->GetOption<uint32>("GuildDungeon.FlightApproachSeconds", 180);

    g_meetingStoneWaitSeconds = sConfigMgr->GetOption<uint32>("GuildDungeon.MeetingStoneWaitSeconds", 90);

    LOG_INFO("module.guilddungeon",
        "[GuildDungeon] Enable={}, TickSeconds={}, MinLevel={}, LevelBand={}, MinMembers={}, MaxRunMinutes={}",
        g_enable ? "true" : "false", g_tickSeconds, g_minLevel, g_levelBand, g_minMembers, g_maxRunMinutes);
    LOG_INFO("module.guilddungeon",
        "[GuildDungeon] RealFlightMaster={}, MultiHopTaxi={}, FlightApproachSeconds={}",
        g_realFlightMaster ? "true" : "false", g_multiHopTaxi ? "true" : "false", g_flightApproachSeconds);
    LOG_INFO("module.guilddungeon",
        "[GuildDungeon] MeetingStoneWaitSeconds={}", g_meetingStoneWaitSeconds);

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

bool GuildDungeonCommandScript::HandleStart(ChatHandler* handler, uint32 guildId, Optional<uint32> dungeonId)
{
    std::string err;
    // Omitted dungeon => 0, which StartRun reads as "first enabled catalog entry".
    if (!GuildDungeon::StartRun(guildId, dungeonId.value_or(0), false, err))
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
