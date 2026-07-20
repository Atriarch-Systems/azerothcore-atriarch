#include "mod-ollama-bot-buddy_cohort.h"
#include "mod-ollama-bot-buddy_config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace
{
    std::unordered_set<uint32> g_cyclingGuids;
    time_t g_builtAt = 0;
    uint32 g_builtForSize = 0;

    std::string LowerCopy(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    }

    void Rebuild(uint32 cohortSize)
    {
        g_cyclingGuids.clear();
        g_builtAt = time(nullptr);
        g_builtForSize = cohortSize;

        std::string prefix = sPlayerbotAIConfig.randomBotAccountPrefix;
        if (prefix.empty())
            return;

        std::vector<std::pair<uint32, uint32>> accounts; // (suffix, accountId)
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

        uint32 collected = 0;
        for (auto const& [suffix, accountId] : accounts)
        {
            if (collected >= cohortSize)
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

        LOG_INFO("server.loading", "[BotCohort] Cycling cohort: {} characters across lowest-numbered '{}' accounts (target {}).",
            uint32(g_cyclingGuids.size()), prefix, cohortSize);
    }
}

namespace BotCohort
{
    std::unordered_set<uint32> const& CyclingGuids(uint32 cohortSize, bool forceRebuild)
    {
        time_t now = time(nullptr);
        if (forceRebuild || g_builtAt == 0 || g_builtForSize != cohortSize || (now - g_builtAt) > 6 * 3600)
            Rebuild(cohortSize);
        return g_cyclingGuids;
    }

    std::unordered_set<std::string> ProtectedNamesLower()
    {
        std::unordered_set<std::string> names;
        std::stringstream ss(g_OllamaBotControlBotNames);
        std::string item;
        while (std::getline(ss, item, ','))
        {
            item.erase(0, item.find_first_not_of(" \t"));
            item.erase(item.find_last_not_of(" \t") + 1);
            if (!item.empty())
                names.insert(LowerCopy(item));
        }
        if (!g_OllamaBotControlSeekerName.empty())
            names.insert(LowerCopy(g_OllamaBotControlSeekerName));
        return names;
    }

    bool IsProtectedName(std::string const& name)
    {
        return ProtectedNamesLower().count(LowerCopy(name)) > 0;
    }

    bool IsRandomBotCharacter(Player* bot)
    {
        if (!bot)
            return false;
        std::string prefix = sPlayerbotAIConfig.randomBotAccountPrefix;
        if (prefix.empty())
            return false;
        QueryResult r = LoginDatabase.Query("SELECT username FROM account WHERE id = {}", bot->GetSession()->GetAccountId());
        if (!r)
            return false;
        std::string name = (*r)[0].Get<std::string>();
        return name.compare(0, prefix.size(), prefix) == 0;
    }
}
