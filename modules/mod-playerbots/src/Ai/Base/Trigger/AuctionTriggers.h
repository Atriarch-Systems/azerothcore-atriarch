/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AUCTIONTRIGGERS_H
#define PLAYERBOTS_AUCTIONTRIGGERS_H

#include "Trigger.h"

class PlayerbotAI;

// Fires periodically so unattended bots can list accumulated ITEM_USAGE_AH-tagged loot on the
// auction house, instead of that only ever being reachable via the "s vendor"/"s *" chat command
// (docs/session-improvements-2026-07-21.md, item 6). Paired with AutoAuctionSellAction
// (Ai/Base/Actions/SellAction.h). Real checkInterval (5 minutes, literal ms) so this doesn't join
// the majority of ungated triggers flagged in docs/playerbot-performance.md item 3 - an AH listing
// pass has no reason to run every AI tick.
class AutoAuctionSellTrigger : public Trigger
{
public:
    AutoAuctionSellTrigger(PlayerbotAI* botAI) : Trigger(botAI, "auto auction sell", 5 * 60 * 1000) {}

    bool IsActive() override;
};

#endif
