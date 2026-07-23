/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LFGLEADERANNOUNCEACTION_H
#define PLAYERBOTS_LFGLEADERANNOUNCEACTION_H

#include "Action.h"

#include <string>

class PlayerbotAI;

// Paired with LfgLeaderAnnounceTrigger (LfgTriggers.h) via the "lfg" strategy: the first time a
// real player is present in a bot-led LFG group inside a dungeon, the leader says once, via
// SayToParty, that it will lead unless asked to hand leadership over (docs/bot-economy.md,
// Phase 1f).
//
// The one-shot state is the announcedKey below - "group counter:map:instance" of the last
// announcement - following LfgLatecomerValue's per-instance-reset scheme in spirit: a different
// group, map, or instance id produces a different key, so the announcement naturally re-arms for
// the next run without any explicit reset hook. One action object exists per bot for the
// lifetime of its AiObjectContext, so the member is bot-scoped, and the trigger re-firing every
// pass just lands on the key no-op.
class LfgLeaderAnnounceAction : public Action
{
public:
    LfgLeaderAnnounceAction(PlayerbotAI* botAI) : Action(botAI, "lfg leader announce") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    // group+instance the announcement was last made for; empty until the first one.
    std::string announcedKey;
};

#endif
