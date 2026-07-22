/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SELLACTION_H
#define PLAYERBOTS_SELLACTION_H

#include "InventoryAction.h"

class FindItemVisitor;
class Item;
class PlayerbotAI;

class SellAction : public InventoryAction
{
public:
    SellAction(PlayerbotAI* botAI, std::string const name = "sell") : InventoryAction(botAI, name) {}

    bool Execute(Event event) override;
    void Sell(FindItemVisitor* visitor);
    void Sell(Item* item);
};

// Autonomous counterpart to "s vendor"/"s *": periodically lists a handful of ITEM_USAGE_AH-tagged
// items on the auction house without a chat command, for bots that have no real player master.
// Paired with AutoAuctionSellTrigger (Ai/Base/Trigger/AuctionTriggers.h); wired into
// NonCombatStrategy so it's checked for every bot, gated off for master-controlled bots by
// isUseful() and off entirely unless AiPlayerbot.AutoAuctionSell is enabled.
class AutoAuctionSellAction : public InventoryAction
{
public:
    AutoAuctionSellAction(PlayerbotAI* botAI) : InventoryAction(botAI, "auto auction sell") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
