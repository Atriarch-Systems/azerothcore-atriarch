#include "mod_npc_banter_config.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include <sstream>

// --------------------------------------------
// Feature toggle / core tuning
// --------------------------------------------
bool     g_NpcBanterEnable = false;
uint32_t g_NpcBanterTickSeconds = 15;
float    g_NpcBanterBanterRange = 20.0f;
uint32_t g_NpcBanterMinCooldown = 120;
uint32_t g_NpcBanterMaxCooldown = 300;
uint32_t g_NpcBanterMaxConcurrentQueries = 1;
uint32_t g_NpcBanterCacheReuseChance = 50;
uint32_t g_NpcBanterNumPredict = 25;
bool     g_NpcBanterWhisperOnly = false;
std::vector<std::string> g_NpcBanterBannedTopics;
std::unordered_map<std::string, std::vector<std::string>> g_NpcBanterCannedLines;

std::unordered_map<uint32_t, NpcBanterConfigRow> g_NpcBanterEligibility;
std::unordered_map<std::string, std::string> g_NpcBanterArchetypePrompts;

namespace
{
    std::vector<std::string> SplitAndTrim(std::string const& str, char delim)
    {
        std::vector<std::string> tokens;
        std::stringstream ss(str);
        std::string token;
        while (std::getline(ss, token, delim))
        {
            size_t start = token.find_first_not_of(" \t\r\n");
            size_t end = token.find_last_not_of(" \t\r\n");
            if (start != std::string::npos && end != std::string::npos)
                tokens.push_back(token.substr(start, end - start + 1));
        }
        return tokens;
    }

    // Archetype keys that get a dedicated NpcBanter.CannedLines.<KEY> config
    // entry out of the box; anything else falls back to "Default" (or silence).
    char const* const kBuiltinArchetypeKeys[] = { "GUARD", "VENDOR", "INNKEEPER", "Default" };
}

NpcBanterConfigRow const* FindNpcBanterConfigRow(uint32_t spawnId)
{
    auto it = g_NpcBanterEligibility.find(spawnId);
    if (it == g_NpcBanterEligibility.end())
        return nullptr;
    return &it->second;
}

std::string const& GetNpcBanterArchetypePrompt(std::string const& archetypeKey)
{
    static std::string const empty;
    auto it = g_NpcBanterArchetypePrompts.find(archetypeKey);
    if (it == g_NpcBanterArchetypePrompts.end())
        return empty;
    return it->second;
}

std::vector<std::string> const& GetNpcBanterCannedLines(std::string const& archetypeKey)
{
    static std::vector<std::string> const empty;

    auto it = g_NpcBanterCannedLines.find(archetypeKey);
    if (it != g_NpcBanterCannedLines.end() && !it->second.empty())
        return it->second;

    auto defaultIt = g_NpcBanterCannedLines.find("Default");
    if (defaultIt != g_NpcBanterCannedLines.end())
        return defaultIt->second;

    return empty;
}

void LoadNpcBanterConfig()
{
    g_NpcBanterEnable               = sConfigMgr->GetOption<bool>("NpcBanter.Enable", false);
    g_NpcBanterTickSeconds          = sConfigMgr->GetOption<uint32_t>("NpcBanter.TickSeconds", 15);
    g_NpcBanterBanterRange          = sConfigMgr->GetOption<float>("NpcBanter.BanterRange", 20.0f);
    g_NpcBanterMinCooldown          = sConfigMgr->GetOption<uint32_t>("NpcBanter.MinCooldown", 120);
    g_NpcBanterMaxCooldown          = sConfigMgr->GetOption<uint32_t>("NpcBanter.MaxCooldown", 300);
    g_NpcBanterMaxConcurrentQueries = sConfigMgr->GetOption<uint32_t>("NpcBanter.MaxConcurrentQueries", 1);
    g_NpcBanterCacheReuseChance     = sConfigMgr->GetOption<uint32_t>("NpcBanter.CacheReuseChance", 50);
    g_NpcBanterNumPredict           = sConfigMgr->GetOption<uint32_t>("NpcBanter.NumPredict", 25);
    g_NpcBanterWhisperOnly          = sConfigMgr->GetOption<bool>("NpcBanter.WhisperOnly", false);

    // A misconfigured Max < Min would make urand(min, max) undefined; keep it sane.
    if (g_NpcBanterMaxCooldown < g_NpcBanterMinCooldown)
        g_NpcBanterMaxCooldown = g_NpcBanterMinCooldown;

    g_NpcBanterBannedTopics = SplitAndTrim(
        sConfigMgr->GetOption<std::string>("NpcBanter.BannedTopics", ""), ',');

    g_NpcBanterCannedLines.clear();
    for (char const* key : kBuiltinArchetypeKeys)
    {
        std::string configKey = std::string("NpcBanter.CannedLines.") + key;
        std::vector<std::string> lines = SplitAndTrim(
            sConfigMgr->GetOption<std::string>(configKey, ""), '|');
        if (!lines.empty())
            g_NpcBanterCannedLines[key] = std::move(lines);
    }

    LOG_INFO("module.npcbanter",
        "[NpcBanter] Config loaded: Enable={}, TickSeconds={}, BanterRange={}, Cooldown={}-{}s, "
        "MaxConcurrentQueries={}, CacheReuseChance={}%, NumPredict={}, WhisperOnly={}, BannedTopics={}",
        g_NpcBanterEnable, g_NpcBanterTickSeconds, g_NpcBanterBanterRange,
        g_NpcBanterMinCooldown, g_NpcBanterMaxCooldown, g_NpcBanterMaxConcurrentQueries,
        g_NpcBanterCacheReuseChance, g_NpcBanterNumPredict, g_NpcBanterWhisperOnly,
        uint32_t(g_NpcBanterBannedTopics.size()));
}

void LoadNpcBanterEligibilityFromDB()
{
    g_NpcBanterEligibility.clear();

    QueryResult result = WorldDatabase.Query(
        "SELECT guid, entry, enabled, archetype_key, backstory FROM mod_npc_banter_config");
    if (!result)
    {
        LOG_INFO("module.npcbanter",
            "[NpcBanter] No rows in mod_npc_banter_config - no NPCs are opted in to banter yet.");
        return;
    }

    do
    {
        Field* f = result->Fetch();
        NpcBanterConfigRow row;
        row.guid         = f[0].Get<uint32>();
        row.entry        = f[1].Get<uint32>();
        row.enabled      = f[2].Get<uint8>() != 0;
        row.archetypeKey = f[3].Get<std::string>();
        row.backstory    = f[4].Get<std::string>();
        g_NpcBanterEligibility[row.guid] = std::move(row);
    } while (result->NextRow());

    LOG_INFO("module.npcbanter", "[NpcBanter] Loaded {} eligibility row(s) from mod_npc_banter_config.",
        uint32(g_NpcBanterEligibility.size()));
}

void LoadNpcBanterArchetypesFromDB()
{
    g_NpcBanterArchetypePrompts.clear();

    QueryResult result = WorldDatabase.Query("SELECT `key`, `prompt` FROM mod_npc_banter_archetypes");
    if (!result)
    {
        LOG_ERROR("module.npcbanter",
            "[NpcBanter] No archetype prompts found in mod_npc_banter_archetypes - "
            "has the module's SQL file been sourced into the world database?");
        return;
    }

    do
    {
        Field* f = result->Fetch();
        std::string key    = f[0].Get<std::string>();
        std::string prompt = f[1].Get<std::string>();
        g_NpcBanterArchetypePrompts[key] = prompt;
    } while (result->NextRow());

    LOG_INFO("module.npcbanter", "[NpcBanter] Loaded {} archetype prompt(s).",
        uint32(g_NpcBanterArchetypePrompts.size()));
}

// Definition of the configuration WorldScript.
NpcBanterConfigWorldScript::NpcBanterConfigWorldScript() : WorldScript("NpcBanterConfigWorldScript") { }

void NpcBanterConfigWorldScript::OnBeforeWorldInitialized()
{
    LoadNpcBanterConfig();
    LoadNpcBanterArchetypesFromDB();
    LoadNpcBanterEligibilityFromDB();
}
