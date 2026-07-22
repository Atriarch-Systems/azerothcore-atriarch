#ifndef MOD_NPC_BANTER_PROMPT_H
#define MOD_NPC_BANTER_PROMPT_H

#include <string>

// Builds the full prompt text sent to the LLM: archetype persona + optional
// per-spawn backstory + a fixed system-style instruction pinning the NPC to
// Wrath-era Azeroth and forbidding contradicting its own class/location/role
// (the same instruction class OllamaChat.ChatPromptTemplate already uses).
//
// This instruction rides inside the prompt body itself rather than the
// request's separate "system" field: NpcBanter dispatches through
// mod-ollama-chat's shared g_queryManager.submitQuery()/QueryOllamaAPI(),
// which defaults the system/think fields from mod-ollama-chat's OWN
// OllamaChat.SystemPrompt/think config. NpcBanter.NumPredict, however, IS
// sent as the request's real num_predict via the OllamaQueryOptions override
// passed at dispatch (mod_npc_banter_worldscript.cpp) - the word count woven
// into the prompt text below is the advisory half of that same budget.
std::string BuildNpcBanterPrompt(std::string const& archetypeKey, std::string const& backstory,
                                  std::string const& npcName, std::string const& playerName);

// Post-generation safety net: trims whitespace, strips a single pair of
// wrapping quotes, caps length, and trims a clipped/mid-word-truncated reply
// back to a sentence (or word) boundary. Does NOT apply the banned-topics
// filter - see NpcBanterMatchesBannedTopic for that.
std::string SanitizeNpcBanterResponse(std::string text);

// True if text matches any entry in NpcBanter.BannedTopics. On a match the
// caller must discard the line and fall back to a canned line - never regenerate.
bool NpcBanterMatchesBannedTopic(std::string const& text);

// Small recent-line cache, keyed by archetype key, backing NpcBanter.CacheReuseChance.
// Thread-safe: RememberNpcBanterLine is called from the LLM worker thread,
// PickNpcBanterCachedLine from the world thread.
void RememberNpcBanterLine(std::string const& archetypeKey, std::string const& text);
std::string PickNpcBanterCachedLine(std::string const& archetypeKey);

// Static config-driven fallback line for an archetype (NpcBanter.CannedLines.*).
// Returns an empty string if none is configured for this archetype (and no
// "Default" bucket exists either) - callers must treat that as "say nothing".
std::string PickNpcBanterCannedLine(std::string const& archetypeKey);

#endif // MOD_NPC_BANTER_PROMPT_H
