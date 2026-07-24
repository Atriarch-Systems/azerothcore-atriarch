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

// The previously-missing autonomous vendor outflow (docs/bot-economy.md, Phase 6e):
// SellVendorItemsVisitor above was only ever reachable via the master "s vendor" chat command, so
// random bots accumulated vendor junk until the factory wipe. Paired with AutoVendorSellTrigger
// (Ai/Base/Trigger/EconomyTriggers.h) and wired into NonCombatStrategy, this periodically sells to
// a nearby VENDOR-flagged NPC:
//  (a) all ITEM_USAGE_VENDOR items (greys, unworn soulbound - the missing junk outflow), and
//  (b) ITEM_USAGE_AH items whose entry already sits at/over the AiPlayerbot.MaxAuctionsPerItem
//      flood cap (commons the AH doesn't need go to the vendor instead of waiting for a slot);
//  (c) in the same pass, unvendorable (SellPrice == 0) trade goods over that cap are DESTROYED -
//      unless the bot's own known recipes consume the entry (crafting-loop exemption: an enchanter
//      keeps its dusts).
// Derives from SellAction to reuse the real Sell(Item*) mechanism (CMSG_SELL_ITEM through the
// session handler). Gated off for master-controlled bots by isUseful() and off entirely unless
// AiPlayerbot.AutoVendorSell is enabled.
class AutoVendorSellAction : public SellAction
{
public:
    AutoVendorSellAction(PlayerbotAI* botAI) : SellAction(botAI, "auto vendor sell") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
