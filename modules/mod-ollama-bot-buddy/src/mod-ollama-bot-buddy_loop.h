#pragma once
#include "ScriptMgr.h"
#include <string>

class OllamaBotControlLoop : public WorldScript
{
public:
    OllamaBotControlLoop();
    void OnUpdate(uint32 diff) override;
};

// Journey-memory event capture for the ONE seeker bot (docs/seeker-selenwe.md).
// All hooks early-out unless the affected player IS the configured seeker.
class SeekerEventScript : public PlayerScript
{
public:
    SeekerEventScript() : PlayerScript("SeekerEventScript", {
        PLAYERHOOK_ON_LEVEL_CHANGED,
        PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST,
        PLAYERHOOK_ON_PLAYER_JUST_DIED,
    }) {}

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override;
    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override;
    void OnPlayerJustDied(Player* player) override;
};

// Journal an event for the seeker (world thread). Used by SeekerEventScript
// and (via the chat module) conversation capture.
void SeekerJournalEvent(uint64_t guidRaw, std::string eventType, std::string directive, std::string target, std::string say);

void AddBotCommandHistory(Player* bot, const std::string& command);
void AddBotReasoningHistory(Player* bot, const std::string& reasoning);

std::vector<std::string> GetBotCommandHistory(Player* bot);
std::vector<std::string> GetBotReasoningHistory(Player* bot);

std::string EscapeBracesForFmt(const std::string& input);
