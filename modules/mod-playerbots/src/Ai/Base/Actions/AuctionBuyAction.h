/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AUCTIONBUYACTION_H
#define PLAYERBOTS_AUCTIONBUYACTION_H

#include "Action.h"

class PlayerbotAI;

// Autonomous AH buying (docs/bot-economy.md, Phase 3 + 4b), the demand-side counterpart to
// AutoAuctionSellAction (SellAction.h). Paired with AutoAuctionBuyTrigger (EconomyTriggers.h) via
// NonCombatStrategy; requires an auctioneer NPC in interact range, exactly like the sell path.
//
// One pass, fully synchronous within a single AI tick:
//  - iterate the local faction house's auctions with cheap pre-filters (buyout-only,
//    armor/weapon/known-recipe trade goods, usable level, within budget, not own/same-account/
//    already-bid);
//  - spend at most 25 full evaluations per pass (ItemUsageValue upgrade checks for gear, price
//    heuristics for reagents - shared budget across both passes);
//  - buy at most AiPlayerbot.AuctionBuyMaxPerPass items (default 1), BUYOUT ONLY, by synthesizing
//    the real CMSG_AUCTION_PLACE_BID packet through the bot's own session handler - never
//    reimplementing AH economics. Candidates are re-resolved by auction id right before the
//    packet is sent; no AuctionEntry* is held across ticks.
//
// Gear purchases are capped by the "free money for gear" budget, reagent purchases (crafter bots
// only, for recipes the bot actually knows) by "free money for tradeskill". The won item reaches
// the bot's bags via the won-mail outcome hook (BotAuctionOutcomeScript), and the existing
// equip-upgrade machinery takes it from there.
class AuctionBuyAction : public Action
{
public:
    AuctionBuyAction(PlayerbotAI* botAI) : Action(botAI, "auto auction buy") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
