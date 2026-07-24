/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ECONOMYTRIGGERS_H
#define PLAYERBOTS_ECONOMYTRIGGERS_H

#include "Trigger.h"

class PlayerbotAI;

// Demand side of the bot economy (docs/bot-economy.md, Phase 3), mirroring the
// AutoAuctionSellTrigger/AutoAuctionSellAction pair (AuctionTriggers.h, SellAction.h): fires
// periodically for unattended random bots standing near an auctioneer so AuctionBuyAction can
// evaluate the local faction house for buyout-priced gear upgrades (and, for crafter bots,
// cheap reagents - Phase 4b). Real checkInterval (5 minutes, literal ms) - a house scan has no
// reason to run every AI tick. Gated by AiPlayerbot.AutoAuctionBuy (default 1, read inline via
// sConfigMgr like the other new economy keys; a later consolidation pass moves these into
// PlayerbotAIConfig).
class AutoAuctionBuyTrigger : public Trigger
{
public:
    AutoAuctionBuyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "auto auction buy", 5 * 60 * 1000) {}

    bool IsActive() override;
};

// Gives CraftRandomItemAction (CastCustomSpellAction.h) a live autonomous path
// (docs/bot-economy.md, Phase 4a) - previously its only trigger belonged to the inactive legacy
// "rpg" strategy, so the crafting machinery was dead. The action itself no-ops without reagents
// in bags and prefers self-upgrades/skill-ups, so a slow cadence for unattended, out-of-combat
// random bots is all the gating needed. 10-minute literal-ms checkInterval; gated by
// AiPlayerbot.AutoCraft (default 1).
class AutoCraftTrigger : public Trigger
{
public:
    AutoCraftTrigger(PlayerbotAI* botAI) : Trigger(botAI, "auto craft", 10 * 60 * 1000) {}

    bool IsActive() override;
};

// The missing vendor outflow (docs/bot-economy.md, Phase 6e): autonomous vendor-selling never
// fired anywhere before this - SellVendorItemsVisitor was only reachable via the master "s" chat
// command, so random bots accumulated vendor junk until the factory wipe. Fires for unattended
// random bots with a VENDOR-flagged NPC in interact range so AutoVendorSellAction (SellAction.h)
// can sell junk/over-cap commons and destroy unvendorable over-cap materials. Same 5-minute
// literal-ms checkInterval as the buy trigger above. Deliberately NO sellable-item precondition:
// a faithful one would duplicate the action's own usage-tag + flood-cap evaluation, and the
// action no-ops cheaply (two cached inventory-value reads and a bag walk, once per 5 minutes)
// when there's nothing to sell. Gated by AiPlayerbot.AutoVendorSell (default 1, read inline via
// sConfigMgr like the other new economy keys).
class AutoVendorSellTrigger : public Trigger
{
public:
    AutoVendorSellTrigger(PlayerbotAI* botAI) : Trigger(botAI, "auto vendor sell", 5 * 60 * 1000) {}

    bool IsActive() override;
};

#endif
