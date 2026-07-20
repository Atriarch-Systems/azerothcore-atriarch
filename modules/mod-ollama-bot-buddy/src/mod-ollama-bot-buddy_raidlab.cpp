#include "mod-ollama-bot-buddy_raidlab.h"
#include "mod-ollama-bot-buddy_cohort.h"
#include "mod-ollama-bot-buddy_intent.h"
#include "mod-ollama-bot-buddy_config.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Log.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "World.h"
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    uint32 g_raidLabGearScore = 5000;
    uint32 g_raidLabCohortSize = 500; // must match Rebirth.CohortSize

    struct Subject
    {
        uint32 guidLow = 0;
        std::string name;
        std::string role; // tank | healer | dps
    };

    std::vector<Subject> g_subjects;
    bool g_subjectsLoaded = false;

    // Active experiment state
    bool g_running = false;
    std::string g_instanceKey;
    uint32 g_instanceMapId = 0;
    time_t g_startedAt = 0;
    uint32 g_wipes = 0;
    bool g_wipeLatch = false;
    std::unordered_set<uint32> g_deadReported;
    std::unordered_set<uint32> g_bossReported;

    struct RaidLabInstance
    {
        char const* key;
        uint32 mapId;
        char const* label;
        // documented fallback entrance (used only if areatrigger_teleport lookup fails)
        float x, y, z, o;
    };

    // Fallback coordinates are the classic WotLK entrance locations; the
    // authoritative source is areatrigger_teleport (queried first).
    RaidLabInstance const kInstances[] = {
        { "naxx", 533, "Naxxramas",                   3005.68f, -3435.11f, 293.882f, 0.0f },
        { "eoe",  616, "The Eye of Eternity",         1050.60f,  1044.00f, 297.610f, 0.0f },
        { "voa",  624, "Vault of Archavon",           5453.14f,  2840.55f, 418.674f, 0.0f },
        { "icc",  631, "Icecrown Citadel",           -17.0f,     2211.0f,  30.0f,    0.0f },
    };

    RaidLabInstance const* FindInstance(std::string key)
    {
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        for (auto const& i : kInstances)
            if (key == i.key)
                return &i;
        return nullptr;
    }

    // Resolve the entrance from the world DB: the areatrigger that teleports
    // INTO the instance map. Falls back to the documented coordinates above.
    bool ResolveEntrance(RaidLabInstance const& info, float& x, float& y, float& z, float& o, std::string& source)
    {
        if (QueryResult r = WorldDatabase.Query(
            "SELECT target_position_x, target_position_y, target_position_z, target_orientation "
            "FROM areatrigger_teleport WHERE target_map = {} LIMIT 1", info.mapId))
        {
            Field* f = r->Fetch();
            x = f[0].Get<float>();
            y = f[1].Get<float>();
            z = f[2].Get<float>();
            o = f[3].Get<float>();
            source = "areatrigger_teleport";
            return true;
        }
        x = info.x; y = info.y; z = info.z; o = info.o;
        source = "hardcoded fallback";
        return false;
    }

    void LoadSubjects()
    {
        g_subjectsLoaded = true;
        g_subjects.clear();
        if (QueryResult r = CharacterDatabase.Query("SELECT guid, name, role FROM mod_raidlab_subjects ORDER BY id ASC"))
        {
            do
            {
                Field* f = r->Fetch();
                g_subjects.push_back({ f[0].Get<uint32>(), f[1].Get<std::string>(), f[2].Get<std::string>() });
            } while (r->NextRow());
        }
    }

    void SaveSubjects()
    {
        CharacterDatabase.Execute("DELETE FROM mod_raidlab_subjects");
        for (auto const& s : g_subjects)
        {
            std::string name = s.name;
            std::string role = s.role;
            CharacterDatabase.EscapeString(name);
            CharacterDatabase.EscapeString(role);
            CharacterDatabase.Execute("INSERT INTO mod_raidlab_subjects (guid, name, role) VALUES ({}, '{}', '{}')",
                s.guidLow, name, role);
        }
    }

    std::vector<Player*> OnlineSubjects()
    {
        std::vector<Player*> out;
        if (!g_subjectsLoaded)
            LoadSubjects();
        for (auto const& s : g_subjects)
            if (Player* p = ObjectAccessor::FindPlayerByName(s.name))
                if (p->IsInWorld())
                    out.push_back(p);
        return out;
    }

    std::string RoleOf(std::string const& name)
    {
        for (auto const& s : g_subjects)
            if (s.name == name)
                return s.role;
        return "?";
    }

    void Reply(ChatHandler* handler, std::string const& msg)
    {
        handler->SendSysMessage(msg.c_str());
        LOG_INFO("server.loading", "[RaidLab] {}", msg);
    }
}

RaidLabCommandScript::RaidLabCommandScript() : CommandScript("RaidLabCommandScript") {}

ChatCommandTable RaidLabCommandScript::GetCommands() const
{
    static ChatCommandTable raidLabTable =
    {
        { "setup",  HandleSetup,  SEC_ADMINISTRATOR, Console::Yes },
        { "guild",  HandleGuild,  SEC_ADMINISTRATOR, Console::Yes },
        { "start",  HandleStart,  SEC_ADMINISTRATOR, Console::Yes },
        { "status", HandleStatus, SEC_ADMINISTRATOR, Console::Yes },
        { "stop",   HandleStop,   SEC_ADMINISTRATOR, Console::Yes },
        { "route",  HandleRoute,  SEC_ADMINISTRATOR, Console::Yes },
        { "go",     HandleGo,     SEC_ADMINISTRATOR, Console::Yes },
        { "pause",  HandlePause,  SEC_ADMINISTRATOR, Console::Yes },
        { "step",   HandleStep,   SEC_ADMINISTRATOR, Console::Yes },
    };

    static ChatCommandTable commandTable =
    {
        { "raidlab", raidLabTable }
    };

    return commandTable;
}

bool RaidLabCommandScript::HandleSetup(ChatHandler* handler, Optional<uint32> countArg)
{
    uint32 count = countArg ? *countArg : 10;
    if (count < 1 || count > 40)
    {
        Reply(handler, "raidlab setup: count must be 1..40");
        return true;
    }

    // Role targets scale from the canonical 2/3/5 of a 10-man.
    uint32 wantTanks  = std::max<uint32>(1, uint32(std::lround(count * 0.2)));
    uint32 wantHeals  = std::max<uint32>(1, uint32(std::lround(count * 0.3)));
    uint32 wantDps    = count > (wantTanks + wantHeals) ? count - wantTanks - wantHeals : 1;

    auto const& cycling = BotCohort::CyclingGuids(g_raidLabCohortSize);

    // Role capability is decided by CLASS, not by current spec: on a young
    // server every bot is level 1 with no talents, so spec-derived
    // IsTank/IsHeal/IsDps are false for everyone. The lab levels and gears
    // subjects itself (the factory picks talents), so what matters is what a
    // class CAN do once trained.
    auto isPureTank = [](uint8 cls) { return cls == CLASS_WARRIOR || cls == CLASS_DEATH_KNIGHT; };
    auto isPureHeal = [](uint8 cls) { return cls == CLASS_PRIEST  || cls == CLASS_SHAMAN; };
    auto isHybrid   = [](uint8 cls) { return cls == CLASS_PALADIN || cls == CLASS_DRUID; };
    auto tankCapable = [&](uint8 cls) { return isPureTank(cls) || isHybrid(cls); };
    auto healCapable = [&](uint8 cls) { return isPureHeal(cls) || isHybrid(cls); };

    // Eligible pool, bucketed by capability. Hybrids are held back so they can
    // backfill whichever role ends up short instead of stranding pure classes.
    std::vector<Player*> poolPureTank, poolPureHeal, poolHybrid, poolOther;
    uint32 eligibleCount = 0;
    for (auto const& itr : ObjectAccessor::GetPlayers())
    {
        Player* bot = itr.second;
        if (!bot || !bot->IsInWorld())
            continue;
        PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
        if (!ai)
            continue;                                        // real players never touched
        if (BotCohort::IsProtectedName(bot->GetName()))
            continue;                                        // seeker / director bots
        if (cycling.count(bot->GetGUID().GetCounter()))
            continue;                                        // rebirth cohort: never draft
        if (!BotCohort::IsRandomBotCharacter(bot))
            continue;                                        // only random bots
        // Subject preconditions: anything the teleport/group path would choke
        // on later is excluded here rather than failing mid-launch.
        if (bot->IsInCombat() || bot->InBattleground() || bot->IsInFlight() || bot->IsBeingTeleported())
            continue;

        ++eligibleCount;
        uint8 cls = bot->getClass();
        if (isPureTank(cls))      poolPureTank.push_back(bot);
        else if (isPureHeal(cls)) poolPureHeal.push_back(bot);
        else if (isHybrid(cls))   poolHybrid.push_back(bot);
        else                      poolOther.push_back(bot);
    }

    std::vector<Player*> tanks, heals, dps;
    std::unordered_set<uint32> drafted;
    auto draft = [&](std::vector<Player*>& dest, std::vector<Player*>& pool, uint32 want)
    {
        for (Player* p : pool)
        {
            if (dest.size() >= want)
                break;
            if (drafted.count(p->GetGUID().GetCounter()))
                continue;
            drafted.insert(p->GetGUID().GetCounter());
            dest.push_back(p);
        }
    };

    // Tanks: warriors/DKs first, hybrids backfill. Healers: priests/shamans
    // first, hybrids backfill. Then everyone else fills dps.
    draft(tanks, poolPureTank, wantTanks);
    draft(tanks, poolHybrid,   wantTanks);
    draft(heals, poolPureHeal, wantHeals);
    draft(heals, poolHybrid,   wantHeals);
    draft(dps,   poolOther,    wantDps);
    draft(dps,   poolHybrid,   wantDps);
    draft(dps,   poolPureHeal, wantDps);
    draft(dps,   poolPureTank, wantDps);

    if (tanks.size() < wantTanks || heals.size() < wantHeals || dps.size() < wantDps)
        LOG_INFO("server.loading", "[RaidLab] Role shortfall: tanks {}/{}, healers {}/{}, dps {}/{} - filling best effort.",
            uint32(tanks.size()), wantTanks, uint32(heals.size()), wantHeals, uint32(dps.size()), wantDps);

    g_subjects.clear();
    g_subjectsLoaded = true;
    auto add = [&](std::vector<Player*> const& v, char const* role)
    {
        for (Player* p : v)
            g_subjects.push_back({ p->GetGUID().GetCounter(), p->GetName(), role });
    };
    add(tanks, "tank");
    add(heals, "healer");
    add(dps, "dps");

    if (g_subjects.empty())
    {
        if (eligibleCount == 0)
            Reply(handler, "raidlab setup: no eligible long-lived random bots ONLINE - subjects must be logged in, "
                           "on random-bot accounts above the rebirth cohort line, and not seeker/director bots");
        else
            Reply(handler, "raidlab setup: " + std::to_string(eligibleCount) +
                           " eligible bots online but no roles could be filled (tank-capable " +
                           std::to_string(poolPureTank.size() + poolHybrid.size()) + ", heal-capable " +
                           std::to_string(poolPureHeal.size() + poolHybrid.size()) + ", other " +
                           std::to_string(poolOther.size()) + ")");
        return true;
    }

    // Level + gear every subject with the same machinery the rebirth cycler
    // uses, but pinned to max level and epic quality.
    uint32 maxLevel = std::min<uint32>(sPlayerbotAIConfig.randomBotMaxLevel,
                                       sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
    for (auto const& s : g_subjects)
    {
        Player* bot = ObjectAccessor::FindPlayerByName(s.name);
        if (!bot)
            continue;
        sRandomPlayerbotMgr.SetValue(bot, "level", maxLevel);
        PlayerbotFactory factory(bot, maxLevel, ITEM_QUALITY_EPIC, g_raidLabGearScore);
        factory.Randomize(false);
        // Report the spec-derived role the factory's talent choice actually
        // produced, next to the role we drafted it for.
        std::string actual = PlayerbotAI::IsTank(bot, true) ? "tank"
                           : PlayerbotAI::IsHeal(bot, true) ? "healer"
                           : PlayerbotAI::IsDps(bot, true)  ? "dps" : "none";
        LOG_INFO("server.loading", "[RaidLab] Prepared subject '{}' class {} intended {} -> actual spec role {} at level {} (epic, gs limit {}).",
            s.name, uint32(bot->getClass()), s.role, actual, maxLevel, g_raidLabGearScore);
    }

    SaveSubjects();

    std::ostringstream comp;
    comp << "raidlab setup complete: " << g_subjects.size() << " subjects ("
         << tanks.size() << " tank, " << heals.size() << " healer, " << dps.size() << " dps) at level "
         << maxLevel << ", epic gear, gs limit " << g_raidLabGearScore;
    Reply(handler, comp.str());
    return true;
}

bool RaidLabCommandScript::HandleGuild(ChatHandler* handler, Optional<std::string> nameArg)
{
    std::string guildName = nameArg && !nameArg->empty() ? *nameArg : "Lab Subjects";

    std::vector<Player*> subjects = OnlineSubjects();
    if (subjects.empty())
    {
        Reply(handler, "raidlab guild: no subjects online (run .raidlab setup first)");
        return true;
    }

    // Leader: first tank if we have one, else first subject.
    Player* leader = subjects.front();
    for (Player* p : subjects)
        if (RoleOf(p->GetName()) == "tank") { leader = p; break; }

    if (leader->GetGuildId())
    {
        Reply(handler, "raidlab guild: leader already guilded, skipping creation");
        return true;
    }

    // Guild::Create rejects: existing name, leader without a session, and
    // (via the caller) invalid charter names. Spaces ARE allowed
    // (IsValidCharterName -> isValidString with numericOrSpace=true), but
    // length/profanity/reserved rules still apply, so pre-check and report.
    if (guildName.size() < 2 || guildName.size() > 24)
    {
        Reply(handler, "raidlab guild: name must be 2-24 characters");
        return true;
    }
    if (sGuildMgr->GetGuildByName(guildName))
    {
        Reply(handler, "raidlab guild: a guild named '" + guildName + "' already exists");
        return true;
    }
    if (!ObjectMgr::IsValidCharterName(guildName))
    {
        Reply(handler, "raidlab guild: '" + guildName + "' is not a valid guild name (reserved/profanity/charset)");
        return true;
    }

    Guild* guild = new Guild();
    if (!guild->Create(leader, guildName))
    {
        delete guild;
        Reply(handler, "raidlab guild: guild creation failed for leader " + leader->GetName());
        return true;
    }
    sGuildMgr->AddGuild(guild);

    uint32 added = 0;
    for (Player* p : subjects)
    {
        if (p == leader || p->GetGuildId())
            continue;
        if (guild->AddMember(p->GetGUID()))
            ++added;
        else
            LOG_INFO("server.loading", "[RaidLab] Guild add failed for '{}' (already guilded, or cross-faction with the leader).",
                p->GetName());
    }

    Reply(handler, "raidlab guild: '" + guildName + "' created, leader " + leader->GetName() +
          ", members added " + std::to_string(added));
    return true;
}

bool RaidLabCommandScript::HandleStart(ChatHandler* handler, std::string instanceKey)
{
    RaidLabInstance const* info = FindInstance(instanceKey);
    if (!info)
    {
        Reply(handler, "raidlab start: unknown instance (use naxx | eoe | voa | icc)");
        return true;
    }

    std::vector<Player*> subjects = OnlineSubjects();
    if (subjects.empty())
    {
        Reply(handler, "raidlab start: no subjects online (run .raidlab setup first)");
        return true;
    }

    Player* leader = subjects.front();
    for (Player* p : subjects)
        if (RoleOf(p->GetName()) == "tank") { leader = p; break; }

    // Fresh group every launch.
    if (Group* old = leader->GetGroup())
        old->Disband();

    Group* group = new Group();
    if (!group->Create(leader))
    {
        delete group;
        Reply(handler, "raidlab start: group creation failed");
        return true;
    }
    sGroupMgr->AddGroup(group);
    group->ConvertToRaid();
    group->SetRaidDifficulty(RAID_DIFFICULTY_10MAN_NORMAL);
    group->SetDungeonDifficulty(DUNGEON_DIFFICULTY_NORMAL);

    uint32 members = 1;
    for (Player* p : subjects)
    {
        if (p == leader)
            continue;
        if (Group* g = p->GetGroup())
            g->RemoveMember(p->GetGUID());
        if (group->AddMember(p))
            ++members;
        else
            LOG_INFO("server.loading", "[RaidLab] Could not add '{}' to the raid group (raid full?).", p->GetName());
    }

    // Raid maps reject anyone who is not IN a raid group and whose personal
    // raid difficulty does not match (MapMgr::PlayerCannotEnter), so the group
    // must exist and every member's difficulty must be set BEFORE the
    // teleport. Bots also cannot enter dead or in combat.
    for (Player* p : subjects)
    {
        p->SetRaidDifficulty(RAID_DIFFICULTY_10MAN_NORMAL);
        p->SetDungeonDifficulty(DUNGEON_DIFFICULTY_NORMAL);
        p->CombatStop(true);
        if (!p->IsAlive())
            p->ResurrectPlayer(1.0f);
    }

    float x, y, z, o;
    std::string source;
    ResolveEntrance(*info, x, y, z, o, source);

    uint32 teleported = 0, refused = 0;
    for (Player* p : subjects)
    {
        if (!p->GetGroup() || !p->GetGroup()->isRaidGroup())
        {
            ++refused;
            LOG_INFO("server.loading", "[RaidLab] '{}' is not in the raid group - skipping teleport (raid maps require a raid group).", p->GetName());
            continue;
        }
        if (p->TeleportTo(info->mapId, x, y, z, o))
            ++teleported;
        else
        {
            ++refused;
            LOG_INFO("server.loading", "[RaidLab] TeleportTo({}) REFUSED for '{}' - check instance bind/difficulty/level gates.",
                info->mapId, p->GetName());
        }
    }
    LOG_INFO("server.loading", "[RaidLab] Teleport results: {} accepted, {} refused.", teleported, refused);

    g_running = true;
    g_instanceKey = info->key;
    g_instanceMapId = info->mapId;
    g_startedAt = time(nullptr);
    g_wipes = 0;
    g_wipeLatch = false;
    g_deadReported.clear();
    g_bossReported.clear();

    std::ostringstream msg;
    msg << "raidlab start: " << info->label << " (map " << info->mapId << ") - raid of " << members
        << ", leader " << leader->GetName() << ", 10-man normal, entrance from " << source;
    Reply(handler, msg.str());

    // AiPlayerbot.ApplyInstanceStrategies does NOT fire when we teleport bots
    // in ourselves (verified live: only generic combat strategies attached),
    // so apply the per-instance set explicitly and log the result.
    for (Player* p : subjects)
    {
        std::string strategies = Intent::ApplyInstanceStrategies(p);
        LOG_INFO("server.loading", "[RaidLab] {} combat strategies after explicit apply: {}", p->GetName(), strategies);
    }

    // Arm the intent engine with this group; progression starts on .raidlab go.
    {
        std::vector<ObjectGuid> guids;
        for (Player* p : subjects)
            guids.push_back(p->GetGUID());
        Intent::StartRun(info->mapId, guids, 1);
        Intent::PauseRun(); // explicit .raidlab go begins movement
        if (Intent::RouteSize() == 0)
            LOG_INFO("server.loading", "[RaidLab] No route data for map {} - bots will hold at the entrance until a route is seeded.", info->mapId);
        else
            Reply(handler, "raidlab: route loaded (" + std::to_string(Intent::RouteSize()) +
                           " steps). Use '.raidlab go' to begin progression.");
    }
    return true;
}

bool RaidLabCommandScript::HandleRoute(ChatHandler* handler)
{
    uint32 mapId = Intent::IsActive() ? Intent::RunMapId() : 0;
    auto const& route = Intent::GetRoute(mapId);
    if (route.empty())
    {
        Reply(handler, "raidlab route: no route loaded (run .raidlab start first, and seed mod_instance_route for the map)");
        return true;
    }

    for (auto const& wp : route)
    {
        std::ostringstream line;
        line << (wp.step == Intent::CurrentStep() ? "-> " : "   ")
             << wp.step << ". " << wp.label << " [" << wp.kind << "] ("
             << uint32(wp.x) << ", " << uint32(wp.y) << ", " << uint32(wp.z) << ")";
        handler->SendSysMessage(line.str().c_str());
    }

    std::ostringstream sum;
    sum << "raidlab route: map " << mapId << ", step " << Intent::CurrentStep() << "/" << route.size()
        << ", status " << Intent::StatusName(Intent::CurrentStatus());
    Reply(handler, sum.str());
    return true;
}

bool RaidLabCommandScript::HandleGo(ChatHandler* handler)
{
    if (!Intent::IsActive())
    {
        Reply(handler, "raidlab go: no run armed (use .raidlab start <instance> first)");
        return true;
    }
    Intent::ResumeRun();
    Reply(handler, "raidlab go: progression running from step " + std::to_string(Intent::CurrentStep()));
    return true;
}

bool RaidLabCommandScript::HandlePause(ChatHandler* handler)
{
    if (!Intent::IsActive())
    {
        Reply(handler, "raidlab pause: no run active");
        return true;
    }
    Intent::PauseRun();
    Reply(handler, "raidlab pause: progression paused at step " + std::to_string(Intent::CurrentStep()));
    return true;
}

bool RaidLabCommandScript::HandleStep(ChatHandler* handler, uint32 step)
{
    if (!Intent::IsActive())
    {
        Reply(handler, "raidlab step: no run active");
        return true;
    }
    Intent::JumpToStep(step);
    Reply(handler, "raidlab step: jumped to step " + std::to_string(Intent::CurrentStep()));
    return true;
}

bool RaidLabCommandScript::HandleStatus(ChatHandler* handler)
{
    std::vector<Player*> subjects = OnlineSubjects();
    if (subjects.empty())
    {
        Reply(handler, "raidlab status: no subjects online");
        return true;
    }

    uint32 alive = 0;
    for (Player* p : subjects)
    {
        if (p->IsAlive())
            ++alive;
        std::string targetName = "-";
        if (Unit* t = p->GetVictim())
            targetName = t->GetName();
        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(p->GetZoneId());

        std::ostringstream line;
        line << p->GetName() << " | " << RoleOf(p->GetName()) << " | lvl " << uint32(p->GetLevel())
             << " | " << (p->IsAlive() ? "alive" : "DEAD")
             << " | hp " << uint32(p->GetHealthPct()) << "%"
             << " | map " << p->GetMapId() << " inst " << p->GetInstanceId()
             << " | " << (zone && zone->area_name[0] ? zone->area_name[0] : "?")
             << " | " << (p->IsInCombat() ? "in combat" : "idle")
             << " | target " << targetName;
        handler->SendSysMessage(line.str().c_str());
    }

    std::ostringstream sum;
    sum << "raidlab status: alive " << alive << "/" << subjects.size()
        << " | intent " << Intent::StatusName(Intent::CurrentStatus())
        << " step " << Intent::CurrentStep()
        << " | wipes " << g_wipes
        << " | instance " << (g_running ? g_instanceKey : std::string("none"))
        << " | elapsed " << (g_running ? uint32(time(nullptr) - g_startedAt) : 0) << "s";
    Reply(handler, sum.str());
    return true;
}

bool RaidLabCommandScript::HandleStop(ChatHandler* handler)
{
    std::vector<Player*> subjects = OnlineSubjects();

    // Teleport OUT first: leaving the raid group before the teleport would
    // make the outbound trip a non-raid exit from a raid map, and losing the
    // group mid-flight has caused stuck subjects. Capital cities are normal
    // maps, so no group is required for the exit itself - but do it in this
    // order so subjects are never grouped-but-stranded.
    for (Player* p : subjects)
    {
        p->CombatStop(true);
        if (!p->IsAlive())
            p->ResurrectPlayer(1.0f);
        if (p->GetTeamId() == TEAM_ALLIANCE)
            p->TeleportTo(0, -8833.38f, 628.628f, 94.0066f, 1.0f);
        else
            p->TeleportTo(1, 1503.32f, -4415.35f, 21.7195f, 3.14f);
    }

    for (Player* p : subjects)
    {
        if (Group* g = p->GetGroup())
        {
            g->Disband();
            break; // disbanding once removes everyone
        }
    }

    Intent::StopRun();
    g_running = false;
    g_deadReported.clear();
    g_bossReported.clear();

    Reply(handler, "raidlab stop: group disbanded, " + std::to_string(subjects.size()) + " subjects returned to capitals");
    return true;
}

// ---------------------------------------------------------------------------
// Watcher: periodic summary, death and wipe detection
// ---------------------------------------------------------------------------

RaidLabWatcher::RaidLabWatcher() : WorldScript("RaidLabWatcher") {}

void RaidLabWatcher::OnStartup()
{
    g_raidLabGearScore = sConfigMgr->GetOption<uint32>("RaidLab.GearScore", 5000);
    g_raidLabCohortSize = sConfigMgr->GetOption<uint32>("Rebirth.CohortSize", 500);

    if (const char* v = std::getenv("AC_RAIDLAB_GEARSCORE"))
        if (*v)
            g_raidLabGearScore = static_cast<uint32>(std::strtoul(v, nullptr, 10));

    LOG_INFO("server.loading", "[RaidLab] Ready. GearScore={}, cohortSize={} (subjects are drawn above this line).",
        g_raidLabGearScore, g_raidLabCohortSize);
}

void RaidLabWatcher::OnUpdate(uint32 /*diff*/)
{
    if (!g_running)
        return;

    static time_t nextTick = 0;
    time_t now = time(nullptr);
    if (now < nextTick)
        return;
    nextTick = now + 30;

    std::vector<Player*> subjects = OnlineSubjects();
    if (subjects.empty())
        return;

    uint32 alive = 0, inCombat = 0, inInstance = 0;
    for (Player* p : subjects)
    {
        if (p->IsAlive())
            ++alive;
        else if (!g_deadReported.count(p->GetGUID().GetCounter()))
        {
            g_deadReported.insert(p->GetGUID().GetCounter());
            LOG_INFO("server.loading", "[RaidLab][Death] {} ({}) died in {}.", p->GetName(), RoleOf(p->GetName()), g_instanceKey);
        }
        if (p->IsAlive())
            g_deadReported.erase(p->GetGUID().GetCounter()); // resurrected: allow future reports

        if (p->IsInCombat())
            ++inCombat;
        if (p->GetMapId() == g_instanceMapId)
            ++inInstance;

        // Best-effort boss engagement detection: a subject fighting a
        // world-boss/elite-classified creature inside the instance.
        if (Unit* victim = p->GetVictim())
        {
            if (Creature* c = victim->ToCreature())
            {
                bool boss = c->GetCreatureTemplate() &&
                    (c->GetCreatureTemplate()->rank == CREATURE_ELITE_WORLDBOSS ||
                     c->GetCreatureTemplate()->rank == CREATURE_ELITE_RAREELITE ||
                     c->isWorldBoss());
                if (boss && p->GetMapId() == g_instanceMapId && !g_bossReported.count(c->GetEntry()))
                {
                    g_bossReported.insert(c->GetEntry());
                    LOG_INFO("server.loading", "[RaidLab][Boss] Engagement started: '{}' (entry {}) in {}.",
                        c->GetName(), c->GetEntry(), g_instanceKey);
                }
            }
        }
    }

    if (alive == 0 && !g_wipeLatch)
    {
        g_wipeLatch = true;
        ++g_wipes;
        LOG_INFO("server.loading", "[RaidLab][Wipe] Full raid wipe #{} in {} after {}s.",
            g_wipes, g_instanceKey, uint32(now - g_startedAt));
    }
    else if (alive > 0)
        g_wipeLatch = false;

    LOG_INFO("server.loading", "[RaidLab][Tick] alive={}/{} inCombat={} inInstance={} map={} instance={} elapsed={}s wipes={}",
        alive, uint32(subjects.size()), inCombat, inInstance, g_instanceMapId, g_instanceKey,
        uint32(now - g_startedAt), g_wipes);
}
