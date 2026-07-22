/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AuctionTriggers.h"

#include "ItemUsageValue.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"

bool AutoAuctionSellTrigger::IsActive()
{
    if (!sPlayerbotAIConfig.autoAuctionSell)
        return false;

    // Cheap short-circuit before the item/NPC scans below - AutoAuctionSellAction::isUseful()
    // enforces this same rule authoritatively, this is just to avoid the work for the common
    // master-controlled-bot case on a trigger that isn't shared with anything else.
    if (botAI->HasActivePlayerMaster())
        return false;

    if (!AI_VALUE2(uint32, "item count", "usage " + std::to_string(ITEM_USAGE_AH)))
        return false;

    // AuctionItem() posts through AuctionHouseMgr exactly like a real "Auctioneer" gossip window -
    // it needs a live auctioneer NPC in range, there's no remote-listing API (see the comment on
    // StoreLootAction::AuctionItem, Ai/Base/Actions/LootAction.cpp).
    GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
    for (ObjectGuid const guid : npcs)
    {
        if (bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_AUCTIONEER))
            return true;
    }

    return false;
}
