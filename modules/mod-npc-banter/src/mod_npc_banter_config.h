#ifndef MOD_NPC_BANTER_CONFIG_H
#define MOD_NPC_BANTER_CONFIG_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "ScriptMgr.h" // WorldScript

// --------------------------------------------
// Feature toggle / core tuning (docs/npc-banter.md config table)
// --------------------------------------------
extern bool     g_NpcBanterEnable;
extern uint32_t g_NpcBanterTickSeconds;
extern float    g_NpcBanterBanterRange;
extern uint32_t g_NpcBanterMinCooldown;
extern uint32_t g_NpcBanterMaxCooldown;
extern uint32_t g_NpcBanterMaxConcurrentQueries;
extern uint32_t g_NpcBanterCacheReuseChance;
extern uint32_t g_NpcBanterNumPredict;
extern bool     g_NpcBanterWhisperOnly;
extern std::vector<std::string> g_NpcBanterBannedTopics;

// Pipe-separated canned-line fallback per archetype key ("GUARD" / "VENDOR" /
// "INNKEEPER"), plus a "Default" bucket used when an archetype has no list of
// its own. docs/npc-banter.md section 4 requires this safe-degrade path ("a
// config-driven pipe-separated list per archetype") but its config table does
// not name a specific key for it - see the module's conf/*.dist file and the
// Deviations note in the delivering agent's report for why these
// NpcBanter.CannedLines.* keys were added.
extern std::unordered_map<std::string, std::vector<std::string>> g_NpcBanterCannedLines;

// --------------------------------------------
// Eligibility (mod_npc_banter_config) - loaded once at startup, keyed by
// creature.guid (the specific spawn, not the shared creature_template.entry).
// --------------------------------------------
struct NpcBanterConfigRow
{
    uint32_t    guid = 0;          // creature.guid (spawn id)
    uint32_t    entry = 0;         // creature_template.entry, validated at registration time
    bool        enabled = true;
    std::string archetypeKey = "GUARD";
    std::string backstory;         // optional per-spawn flavor override
};

extern std::unordered_map<uint32_t, NpcBanterConfigRow> g_NpcBanterEligibility;

// --------------------------------------------
// Archetype prompts (mod_npc_banter_archetypes) - loaded once at startup.
// --------------------------------------------
extern std::unordered_map<std::string, std::string> g_NpcBanterArchetypePrompts;

NpcBanterConfigRow const* FindNpcBanterConfigRow(uint32_t spawnId);
std::string const& GetNpcBanterArchetypePrompt(std::string const& archetypeKey);
std::vector<std::string> const& GetNpcBanterCannedLines(std::string const& archetypeKey);

// --------------------------------------------
// Loader functions
// --------------------------------------------
void LoadNpcBanterConfig();
void LoadNpcBanterEligibilityFromDB();
void LoadNpcBanterArchetypesFromDB();

// --------------------------------------------
// Declaration of the configuration WorldScript.
// --------------------------------------------
class NpcBanterConfigWorldScript : public WorldScript
{
public:
    NpcBanterConfigWorldScript();

    // Deliberately NOT OnStartup(): sScriptMgr->OnStartup() (Main.cpp) runs
    // AFTER World::SetInitialWorldSettings(), and when
    // PreloadAllNonInstancedMapGrids = 1 that same function eagerly loads
    // every non-instanced map's grids - and therefore fires
    // AllCreatureScript::OnCreatureAddWorld() for every guard/vendor/
    // innkeeper on the whole continent - BEFORE OnStartup() would ever run.
    // Since a grid's creatures are only added to the world once (grids do not
    // reload while the server is up), any NPC swept up in that eager preload
    // would silently never see g_NpcBanterEligibility/g_NpcBanterArchetypePrompts
    // populated and would never register for banter for the server's entire
    // uptime. OnBeforeWorldInitialized() fires earlier still - inside
    // SetInitialWorldSettings(), immediately before that preload block - so
    // this data is guaranteed loaded first regardless of that config setting.
    void OnBeforeWorldInitialized() override;
};

#endif // MOD_NPC_BANTER_CONFIG_H
