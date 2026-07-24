/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EconomyTriggers.h"

#include "Config.h"
#include "Playerbots.h"

bool AutoAuctionBuyTrigger::IsActive()
{
    // Inline config read cached in a function-local static; a later consolidation pass will move
    // this into PlayerbotAIConfig (same convention as AiPlayerbot.Lfg.SummonRetryMinutes in
    // LfgLatecomerAction.cpp).
    static bool const autoAuctionBuy = sConfigMgr->GetOption<bool>("AiPlayerbot.AutoAuctionBuy", true);
    if (!autoAuctionBuy)
        return false;

    // Cheap short-circuit before the NPC scan below - AuctionBuyAction::isUseful() enforces this
    // same rule authoritatively, this is just to avoid the work for the common
    // master-controlled-bot case on a trigger that isn't shared with anything else.
    if (botAI->HasActivePlayerMaster())
        return false;

    // Random bots only, mirroring AuctionBuyAction::isUseful(): a personal (non-random) bot whose
    // owner is merely offline also has no active master, and must not spend its owner's gold
    // behind their back.
    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    // Buying goes through the real CMSG_AUCTION_PLACE_BID session handler, which (exactly like a
    // real "Auctioneer" gossip window) needs a live auctioneer NPC in interact range - same scan
    // as AutoAuctionSellTrigger (AuctionTriggers.cpp).
    GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
    for (ObjectGuid const guid : npcs)
    {
        if (bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_AUCTIONEER))
            return true;
    }

    return false;
}

bool AutoCraftTrigger::IsActive()
{
    static bool const autoCraft = sConfigMgr->GetOption<bool>("AiPlayerbot.AutoCraft", true);
    if (!autoCraft)
        return false;

    if (bot->IsInCombat())
        return false;

    if (botAI->HasActivePlayerMaster())
        return false;

    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    // No reagent/recipe precondition here: CraftRandomItemAction already returns false from its
    // spell-list pass when nothing is craftable, and doing that check properly would cost the
    // same spell-map walk the action does anyway - once per 10 minutes is fine.
    return true;
}

bool AutoVendorSellTrigger::IsActive()
{
    static bool const autoVendorSell = sConfigMgr->GetOption<bool>("AiPlayerbot.AutoVendorSell", true);
    if (!autoVendorSell)
        return false;

    // Cheap short-circuits before the NPC scan below - AutoVendorSellAction::isUseful() enforces
    // the same rules authoritatively (SellAction.cpp), this just avoids the work for the common
    // master-controlled-bot case on a trigger that isn't shared with anything else.
    if (botAI->HasActivePlayerMaster())
        return false;

    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    // Selling goes through the real CMSG_SELL_ITEM session handler, which (exactly like a real
    // vendor gossip window) needs a live VENDOR-flagged NPC in interact range - the same
    // "nearest npcs" + GetNPCIfCanInteractWith scan SellAction::Sell (SellAction.cpp) uses to
    // locate its vendor, with UNIT_NPC_FLAG_VENDOR instead of the auction triggers' AUCTIONEER.
    // No sellable-item precondition (see the class comment, EconomyTriggers.h): the action no-ops
    // cheaply when the bags hold nothing disposable.
    GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
    for (ObjectGuid const guid : npcs)
    {
        if (bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_VENDOR))
            return true;
    }

    return false;
}
