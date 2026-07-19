#include "mod-ollama-bot-buddy_rebirth.h"
#include "mod-ollama-bot-buddy_config.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "World.h"
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    bool   g_rebirthEnable = true;
    uint32 g_rebirthPerDay = 8;
    uint32 g_rebirthCohortSize = 500;

    // Cycling cohort: character GUID counters, refreshed periodically.
    std::unordered_set<uint32> g_cyclingGuids;
    time_t g_cohortBuiltAt = 0;

    // last_rebirth epochs (0 = never reborn -> highest priority)
    std::unordered_map<uint32, time_t> g_lastRebirth;
    bool g_rebirthStateLoaded = false;
    time_t g_lastRebirthGlobal = 0;

    std::string RebirthGetEnv(const char* name)
    {
        if (const char* v = std::getenv(name)) return v;
        return {};
    }

    std::string ToLowerStr(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    }

    // Protected names: every LLM-driven bot (director list + seeker).
    std::unordered_set<std::string> ProtectedNames()
    {
        std::unordered_set<std::string> names;
        std::stringstream ss(g_OllamaBotControlBotNames);
        std::string item;
        while (std::getline(ss, item, ','))
        {
            item.erase(0, item.find_first_not_of(" \t"));
            item.erase(item.find_last_not_of(" \t") + 1);
            if (!item.empty())
                names.insert(ToLowerStr(item));
        }
        if (!g_OllamaBotControlSeekerName.empty())
            names.insert(ToLowerStr(g_OllamaBotControlSeekerName));
        return names;
    }

    // Deterministic cohort partition (documented in conf.dist):
    // random-bot accounts sorted by their numeric suffix ascending; walk that
    // order accumulating character counts; accounts encountered before the
    // cumulative count reaches Rebirth.CohortSize form the CYCLING cohort
    // (all their characters). Later accounts are long-lived. Stable across
    // restarts because account names/ids never change.
    void RebuildCohort()
    {
        g_cyclingGuids.clear();
        g_cohortBuiltAt = time(nullptr);

        std::string prefix = sPlayerbotAIConfig.randomBotAccountPrefix;
        if (prefix.empty())
            return;

        // 1) account ids ordered by numeric suffix
        std::vector<std::pair<uint32 /*suffix*/, uint32 /*accountId*/>> accounts;
        std::string like = prefix + "%";
        LoginDatabase.EscapeString(like);
        if (QueryResult r = LoginDatabase.Query("SELECT id, username FROM account WHERE username LIKE '{}'", like))
        {
            do
            {
                Field* f = r->Fetch();
                uint32 id = f[0].Get<uint32>();
                std::string name = f[1].Get<std::string>();
                uint32 suffix = static_cast<uint32>(std::strtoul(name.c_str() + prefix.size(), nullptr, 10));
                accounts.emplace_back(suffix, id);
            } while (r->NextRow());
        }
        std::sort(accounts.begin(), accounts.end());

        // 2) walk accounts in order, collecting characters until CohortSize
        uint32 collected = 0;
        for (auto const& [suffix, accountId] : accounts)
        {
            if (collected >= g_rebirthCohortSize)
                break;
            if (QueryResult r = CharacterDatabase.Query("SELECT guid FROM characters WHERE account = {}", accountId))
            {
                do
                {
                    g_cyclingGuids.insert((*r)[0].Get<uint32>());
                    ++collected;
                } while (r->NextRow());
            }
        }

        LOG_INFO("server.loading", "[BotRebirth] Cycling cohort built: {} characters across lowest-numbered '{}' accounts (target {}).",
            uint32(g_cyclingGuids.size()), prefix, g_rebirthCohortSize);
    }

    void LoadRebirthState()
    {
        g_rebirthStateLoaded = true;
        if (QueryResult r = CharacterDatabase.Query("SELECT guid, last_rebirth FROM mod_bot_rebirth"))
        {
            do
            {
                Field* f = r->Fetch();
                time_t at = static_cast<time_t>(f[1].Get<uint64>());
                g_lastRebirth[f[0].Get<uint32>()] = at;
                g_lastRebirthGlobal = std::max(g_lastRebirthGlobal, at);
            } while (r->NextRow());
        }
        LOG_INFO("server.loading", "[BotRebirth] Loaded {} rebirth records (last at {}).",
            uint32(g_lastRebirth.size()), uint64(g_lastRebirthGlobal));
    }

    void DoRebirth(Player* bot)
    {
        uint32 oldLevel = bot->GetLevel();
        uint32 level = bot->getClass() == CLASS_DEATH_KNIGHT
            ? sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL)
            : 1;

        LOG_INFO("server.loading", "[BotRebirth] Graduation: '{}' completed the journey (level {} -> {}).", bot->GetName(), oldLevel, level);

        // Same mechanism RandomPlayerbotMgr::RandomizeFirst uses, pinned low:
        // the factory handles leveling, gear, spells, talents for the target level.
        sRandomPlayerbotMgr.SetValue(bot, "level", level);
        PlayerbotFactory factory(bot, level);
        factory.Randomize(false);

        time_t now = time(nullptr);
        uint32 guidLow = bot->GetGUID().GetCounter();
        g_lastRebirth[guidLow] = now;
        g_lastRebirthGlobal = now;
        CharacterDatabase.Execute(
            "REPLACE INTO mod_bot_rebirth (guid, last_rebirth, rebirth_count) VALUES ({}, {}, "
            "IFNULL((SELECT rc FROM (SELECT rebirth_count AS rc FROM mod_bot_rebirth WHERE guid = {}) AS t), 0) + 1)",
            guidLow, uint64(now), guidLow);
    }
}

BotRebirthLoop::BotRebirthLoop() : WorldScript("BotRebirthLoop") {}

void BotRebirthLoop::OnStartup()
{
    g_rebirthEnable = sConfigMgr->GetOption<bool>("Rebirth.Enable", true);
    g_rebirthPerDay = sConfigMgr->GetOption<uint32>("Rebirth.PerDay", 8);
    g_rebirthCohortSize = sConfigMgr->GetOption<uint32>("Rebirth.CohortSize", 500);

    if (auto v = RebirthGetEnv("AC_REBIRTH_ENABLE"); !v.empty())
        g_rebirthEnable = (v == "1" || v == "true" || v == "TRUE");
    if (auto v = RebirthGetEnv("AC_REBIRTH_PERDAY"); !v.empty())
        g_rebirthPerDay = static_cast<uint32>(std::strtoul(v.c_str(), nullptr, 10));
    if (auto v = RebirthGetEnv("AC_REBIRTH_COHORTSIZE"); !v.empty())
        g_rebirthCohortSize = static_cast<uint32>(std::strtoul(v.c_str(), nullptr, 10));

    LOG_INFO("server.loading", "[BotRebirth] Enable={}, PerDay={}, CohortSize={}",
        g_rebirthEnable ? "true" : "false", g_rebirthPerDay, g_rebirthCohortSize);
}

void BotRebirthLoop::OnUpdate(uint32 /*diff*/)
{
    if (!g_rebirthEnable || !g_rebirthPerDay)
        return;

    static time_t nextCheck = 0;
    time_t now = time(nullptr);
    if (now < nextCheck)
        return;
    nextCheck = now + 60;

    if (!g_rebirthStateLoaded)
        LoadRebirthState();

    // Rebuild the cohort at startup and every 6h (new bots may appear)
    if (g_cohortBuiltAt == 0 || now - g_cohortBuiltAt > 6 * 3600)
        RebuildCohort();
    if (g_cyclingGuids.empty())
        return;

    time_t interval = time_t(86400) / g_rebirthPerDay;
    // First-ever run: anchor to now instead of bursting through the backlog.
    if (g_lastRebirthGlobal == 0)
    {
        g_lastRebirthGlobal = now;
        CharacterDatabase.Execute("REPLACE INTO mod_bot_rebirth (guid, last_rebirth, rebirth_count) VALUES (0, {}, 0)", uint64(now));
        return;
    }
    if (now - g_lastRebirthGlobal < interval)
        return;

    // GRADUATION: only cohort bots that reached max level are eligible.
    // Max level source: the playerbots random-bot ceiling clamped by the
    // world max-level config (same clamp RandomizeFirst applies).
    uint32 maxLevel = std::min<uint32>(sPlayerbotAIConfig.randomBotMaxLevel,
                                       sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));

    // Candidate: ONLINE cycling bot AT max level, not protected, not in
    // combat, with the oldest last-rebirth (a freshly-capped bot with no
    // record ranks first: its first graduation).
    auto protectedNames = ProtectedNames();
    Player* best = nullptr;
    time_t bestAt = 0;
    for (auto const& itr : ObjectAccessor::GetPlayers())
    {
        Player* bot = itr.second;
        if (!bot || !bot->IsInWorld() || bot->IsInCombat() || bot->IsBeingTeleported())
            continue;
        PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
        if (!ai)
            continue;
        if (bot->GetLevel() < maxLevel)
            continue; // not graduated yet - NEVER rebirth sub-max bots
        uint32 guidLow = bot->GetGUID().GetCounter();
        if (!g_cyclingGuids.count(guidLow))
            continue;
        if (protectedNames.count(ToLowerStr(bot->GetName())))
            continue;
        auto it = g_lastRebirth.find(guidLow);
        time_t at = (it == g_lastRebirth.end()) ? 0 : it->second;
        if (!best || at < bestAt)
        {
            best = bot;
            bestAt = at;
        }
    }

    if (best)
        DoRebirth(best);
    else
        LOG_DEBUG("server.loading", "[BotRebirth] Interval elapsed but no cohort bot at max level ({}); waiting for a graduation.", maxLevel);
}
