/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotAuctionOutcomeScript.h"

#include "AuctionHouseMgr.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Log.h"
#include "LootAction.h"
#include "Player.h"
#include "RandomPlayerbotMgr.h"

namespace
{
    // Returns the auction party (owner for sold/expired/pending mails, bidder for won mails) as an
    // online random playerbot, or nullptr when the direct-outcome path must not run and the stock
    // mail behavior should be preserved instead:
    //  - feature disabled via config,
    //  - player offline (the hooks' Player* params come from FindConnectedPlayer, so nullptr means
    //    offline) - random bots are ~always online here, so rather than duplicating the module's
    //    offline-gold DB machinery we deliberately fall back to mail for that rare case,
    //  - player is not a random bot (real players and personal/addclass bots keep their mail).
    Player* GetOnlineRandomBot(Player* player)
    {
        static bool const auctionOutcomeDirect = sConfigMgr->GetOption<bool>("AiPlayerbot.AuctionOutcomeDirect", true);
        if (!auctionOutcomeDirect)
            return nullptr;

        if (!player)
            return nullptr;

        if (!sRandomPlayerbotMgr.IsRandomBot(player))
            return nullptr;

        return player;
    }
}

BotAuctionOutcomeScript::BotAuctionOutcomeScript()
    : AuctionHouseScript("BotAuctionOutcomeScript", {
        AUCTIONHOUSEHOOK_ON_BEFORE_AUCTIONHOUSEMGR_SEND_AUCTION_SUCCESSFUL_MAIL,
        AUCTIONHOUSEHOOK_ON_BEFORE_AUCTIONHOUSEMGR_SEND_AUCTION_EXPIRED_MAIL,
        AUCTIONHOUSEHOOK_ON_BEFORE_AUCTIONHOUSEMGR_SEND_AUCTION_WON_MAIL,
        AUCTIONHOUSEHOOK_ON_BEFORE_AUCTIONHOUSEMGR_SEND_AUCTION_SALE_PENDING_MAIL
    })
{
}

void BotAuctionOutcomeScript::OnBeforeAuctionHouseMgrSendAuctionSuccessfulMail(
    AuctionHouseMgr* /*auctionHouseMgr*/, AuctionEntry* auction, Player* owner, uint32& /*owner_accId*/,
    uint32& profit, bool& /*sendNotification*/, bool& /*updateAchievementCriteria*/, bool& sendMail)
{
    Player* bot = GetOnlineRandomBot(owner);
    if (!bot)
        return;

    // Credit the sale profit (bid + deposit - AH cut, already computed by the caller) directly
    // instead of attaching it to a mail the bot will never open. Persist the gold immediately in
    // its own transaction - the same crash-window-minimizing pattern HandleMailTakeMoney uses -
    // rather than waiting for the next periodic bot save.
    bot->ModifyMoney(static_cast<int32>(profit));

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    bot->SaveGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);

    sendMail = false;

    LOG_DEBUG("playerbots", "Bot {} directly credited {} copper for sold auction {} (item {} x{})",
        bot->GetName(), profit, auction->Id, auction->item_template, auction->itemCount);
}

void BotAuctionOutcomeScript::OnBeforeAuctionHouseMgrSendAuctionExpiredMail(
    AuctionHouseMgr* auctionHouseMgr, AuctionEntry* auction, Player* owner, uint32& /*owner_accId*/,
    bool& /*sendNotification*/, bool& sendMail)
{
    Player* bot = GetOnlineRandomBot(owner);
    if (!bot)
        return;

    Item* item = auctionHouseMgr->GetAItem(auction->item_guid);
    if (!item)
        return;

    // Overpopulation item sink: if the house already carries at least the per-item cap
    // (AiPlayerbot.MaxAuctionsPerItem) of this entry - per the shared per-house entry-count cache
    // StoreLootAction::AuctionItem maintains (BotAuctionMarket, LootAction.h) - then nobody wanted
    // this item at cap-level supply, and returning it to bags would only make the bot relist it
    // into the same glut forever. Destroy it instead: FSetState(ITEM_REMOVED) + save deletes it
    // from item_instance and drops it from the manager's map; the caller's later plain
    // RemoveAItem(item_guid) then finds nothing and is a safe no-op. This is the bot economy's
    // only true item destruction, so log it at INFO for observability.
    if (uint32 const maxAuctionsPerItem = BotAuctionMarket::GetMaxAuctionsPerItem())
    {
        uint32 const activeCount =
            BotAuctionMarket::GetCachedAuctionCountForEntry(uint32(auction->houseId), auction->item_template);
        if (activeCount >= maxAuctionsPerItem)
        {
            sendMail = false;

            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            auctionHouseMgr->RemoveAItem(auction->item_guid, true, &trans);
            CharacterDatabase.CommitTransaction(trans);

            LOG_INFO("playerbots", "Bot {}: expired auction {} destroyed as overpopulation item sink (item {} x{}, {} active listings >= cap {})",
                bot->GetName(), auction->Id, auction->item_template, auction->itemCount, activeCount, maxAuctionsPerItem);
            return;
        }
    }

    // Put the unsold item straight back into the bot's bags, mirroring the core's own
    // "take item out of mail" sequence (MailHandler.cpp HandleMailTakeItem): CanStoreItem ->
    // SetState(ITEM_UNCHANGED) -> MoveItemToInventory -> immediate inventory save. The caller
    // (AuctionHouseObject::Update) then erases the map entry via RemoveAItem without deleting the
    // Item object, so ownership transfers cleanly to the player; if the stack merged into an
    // existing one, MoveItemToInventory marked it ITEM_REMOVED and the save below reaps it.
    ItemPosCountVec dest;
    InventoryResult result = bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false);
    if (result != EQUIP_ERR_OK)
    {
        // Bags full: let the normal expired-auction mail carry the item, exactly as today.
        // Bounded leakage (one mail) beats destroying the item outright.
        LOG_DEBUG("playerbots", "Bot {} has no bag space for expired auction {} (item {} x{}), falling back to mail",
            bot->GetName(), auction->Id, auction->item_template, auction->itemCount);
        return;
    }

    item->SetState(ITEM_UNCHANGED);
    bot->MoveItemToInventory(dest, item, true);

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    bot->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);

    sendMail = false;

    LOG_DEBUG("playerbots", "Bot {} directly recovered expired auction {} (item {} x{}) to bags",
        bot->GetName(), auction->Id, auction->item_template, auction->itemCount);
}

void BotAuctionOutcomeScript::OnBeforeAuctionHouseMgrSendAuctionWonMail(
    AuctionHouseMgr* auctionHouseMgr, AuctionEntry* auction, Player* bidder, uint32& /*bidder_accId*/,
    bool& /*sendNotification*/, bool& /*updateAchievementCriteria*/, bool& sendMail)
{
    Player* bot = GetOnlineRandomBot(bidder);
    if (!bot)
        return;

    Item* item = auctionHouseMgr->GetAItem(auction->item_guid);
    if (!item)
        return;

    // Put the won item straight into the bidding bot's bags - same sequence as the expired path
    // above (CanStoreItem -> SetState(ITEM_UNCHANGED) -> MoveItemToInventory -> immediate save).
    // The caller (SendAuctionWonMail) still appends its CHAR_UPD_ITEM_OWNER statement, which just
    // re-writes the owner we already saved, and its later plain RemoveAItem(item_guid) erases the
    // manager's map entry without deleting the Item, so ownership transfers cleanly here too.
    ItemPosCountVec dest;
    InventoryResult result = bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false);
    if (result != EQUIP_ERR_OK)
    {
        // Bags full: let the normal won-auction mail carry the item, exactly as today.
        LOG_DEBUG("playerbots", "Bot {} has no bag space for won auction {} (item {} x{}), falling back to mail",
            bot->GetName(), auction->Id, auction->item_template, auction->itemCount);
        return;
    }

    item->SetState(ITEM_UNCHANGED);
    bot->MoveItemToInventory(dest, item, true);

    // Lifetime caveat unique to the WON path: unlike the expired caller, SendAuctionWonMail still
    // dereferences its pItem pointer AFTER this hook returns (the CHAR_UPD_ITEM_OWNER statement
    // reads pItem->GetGUID()). If the stack merged into an existing one, MoveItemToInventory
    // marked `item` ITEM_REMOVED and queued it, and saving the inventory NOW would reap the queue
    // and delete the Item object (Item::SaveToDB does `delete this` for ITEM_REMOVED) - leaving
    // the caller reading freed memory. So the immediate save only runs in the non-merge case; a
    // merge keeps the queued state and persists on the bot's next periodic save, which is also
    // when the ITEM_REMOVED entry is safely reaped.
    if (item->GetState() != ITEM_REMOVED)
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        bot->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);
    }

    sendMail = false;

    LOG_DEBUG("playerbots", "Bot {} directly received won auction {} (item {} x{}) to bags",
        bot->GetName(), auction->Id, auction->item_template, auction->itemCount);
}

void BotAuctionOutcomeScript::OnBeforeAuctionHouseMgrSendAuctionSalePendingMail(
    AuctionHouseMgr* /*auctionHouseMgr*/, AuctionEntry* /*auction*/, Player* owner, uint32& /*owner_accId*/,
    bool& sendMail)
{
    // Purely informational mail (no money, no items) sent to the owner on buyouts - for a bot that
    // never opens its mailbox this is pure mail-table growth, so drop it.
    if (GetOnlineRandomBot(owner))
        sendMail = false;
}
