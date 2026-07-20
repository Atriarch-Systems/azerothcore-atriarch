#pragma once
#include "Player.h"
#include <string>
#include <unordered_set>

// Shared random-bot population partition, used by BOTH the rebirth cycler and
// the raid lab so they can never disagree about which bots are recyclable.
//
// Partition (deterministic, restart-stable): random-bot accounts
// (AiPlayerbot.RandomBotAccountPrefix + number) ordered by numeric suffix;
// walking that order, whole accounts join the CYCLING cohort until their
// characters total cohortSize. Characters of later ("above the line")
// accounts are LONG-LIVED: they level to cap, stay there, and are the only
// valid raid-lab subjects.
namespace BotCohort
{
    // Character GUID counters of the cycling cohort (rebuilt on demand).
    std::unordered_set<uint32> const& CyclingGuids(uint32 cohortSize, bool forceRebuild = false);

    // Names that must never be recycled or drafted: OllamaBotControl.BotNames
    // plus SeekerName (lower-cased).
    std::unordered_set<std::string> ProtectedNamesLower();

    bool IsProtectedName(std::string const& name);

    // True when the character belongs to a random-bot account.
    bool IsRandomBotCharacter(Player* bot);
}
