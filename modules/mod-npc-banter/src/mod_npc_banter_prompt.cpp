#include "mod_npc_banter_prompt.h"
#include "mod_npc_banter_config.h"
#include "Random.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace
{
    std::string ToLowerCopy(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return char(std::tolower(c)); });
        return s;
    }

    std::mutex g_cacheMutex;
    std::unordered_map<std::string, std::deque<std::string>> g_recentLines; // archetypeKey -> last few lines
    constexpr size_t kMaxCachedLinesPerArchetype = 3;
}

std::string BuildNpcBanterPrompt(std::string const& archetypeKey, std::string const& backstory,
                                  std::string const& npcName, std::string const& playerName)
{
    std::string persona = GetNpcBanterArchetypePrompt(archetypeKey);
    if (persona.empty())
        persona = "A World of Warcraft NPC.";

    std::string prompt = "You are " + npcName + ". " + persona;
    if (!backstory.empty())
        prompt += " " + backstory;

    prompt += " Stay entirely in character as an NPC living in Wrath-era Azeroth (World of Warcraft)."
              " Never mention anything outside the game world, never break character, and never"
              " contradict your own class, race, location, or role.";
    prompt += " A player named " + playerName + " has just come near you.";
    prompt += " Say one short, in-character line of dialogue only - no stage directions, no"
              " markdown, no quotation marks, no more than about " +
              std::to_string(std::max<uint32_t>(6, g_NpcBanterNumPredict)) + " words.";

    return prompt;
}

std::string SanitizeNpcBanterResponse(std::string text)
{
    size_t start = text.find_first_not_of(" \t\r\n");
    size_t end = text.find_last_not_of(" \t\r\n");
    if (start == std::string::npos || end == std::string::npos)
        return "";
    text = text.substr(start, end - start + 1);

    if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
        text = text.substr(1, text.size() - 2);

    // NpcBanter barks are short by design; anything wildly past that is almost
    // certainly the model ignoring the length instruction rather than a
    // legitimate long reply, so it is clipped rather than spoken in full.
    constexpr size_t kMaxLen = 400;
    if (text.size() > kMaxLen)
        text = text.substr(0, kMaxLen);

    return text;
}

bool NpcBanterMatchesBannedTopic(std::string const& text)
{
    if (g_NpcBanterBannedTopics.empty() || text.empty())
        return false;

    std::string lower = ToLowerCopy(text);
    for (std::string const& topic : g_NpcBanterBannedTopics)
    {
        if (topic.empty())
            continue;
        if (lower.find(ToLowerCopy(topic)) != std::string::npos)
            return true;
    }
    return false;
}

void RememberNpcBanterLine(std::string const& archetypeKey, std::string const& text)
{
    if (text.empty())
        return;

    std::lock_guard<std::mutex> lock(g_cacheMutex);
    std::deque<std::string>& lines = g_recentLines[archetypeKey];
    lines.push_back(text);
    while (lines.size() > kMaxCachedLinesPerArchetype)
        lines.pop_front();
}

std::string PickNpcBanterCachedLine(std::string const& archetypeKey)
{
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    auto it = g_recentLines.find(archetypeKey);
    if (it == g_recentLines.end() || it->second.empty())
        return "";

    uint32_t idx = urand(0, uint32_t(it->second.size() - 1));
    return it->second[idx];
}

std::string PickNpcBanterCannedLine(std::string const& archetypeKey)
{
    std::vector<std::string> const& lines = GetNpcBanterCannedLines(archetypeKey);
    if (lines.empty())
        return "";

    uint32_t idx = urand(0, uint32_t(lines.size() - 1));
    return lines[idx];
}
