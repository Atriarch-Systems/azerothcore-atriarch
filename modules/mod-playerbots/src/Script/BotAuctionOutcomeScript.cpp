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
#include "Player.h"
#include "RandomPlayerbotMgr.h"

namespace
{
    // Returns the auction owner as an online random playerbot, or nullptr when the direct-outcome
    // path must not run and the stock mail behavior should be preserved instead:
    //  - feature disabled via config,
    //  - owner offline (the hook's owner param is FindConnectedPlayer(auction->owner), so nullptr
    //    means offline) - random bots are ~always online here, so rather than duplicating the
    //    module's offline-gold DB machinery we deliberately fall back to mail for that rare case,
    //  - owner is not a random bot (real players and personal/addclass bots keep their mail).
    Player* GetOnlineRandomBotOwner(Player* owner)
    {
        static bool const auctionOutcomeDirect = sConfigMgr->GetOption<bool>("AiPlayerbot.AuctionOutcomeDirect", true);
        if (!auctionOutcomeDirect)
            return nullptr;

        if (!owner)
            return nullptr;

        if (!sRandomPlayerbotMgr.IsRandomBot(owner))
            return nullptr;

        return owner;
    }
}

BotAuctionOutcomeScript::BotAuctionOutcomeScript()
    : AuctionHouseScript("BotAuctionOutcomeScript", {
        AUCTIONHOUSEHOOK_ON_BEFORE_AUCTIONHOUSEMGR_SEND_AUCTION_SUCCESSFUL_MAIL,
        AUCTIONHOUSEHOOK_ON_BEFORE_AUCTIONHOUSEMGR_SEND_AUCTION_EXPIRED_MAIL,
        AUCTIONHOUSEHOOK_ON_BEFORE_AUCTIONHOUSEMGR_SEND_AUCTION_SALE_PENDING_MAIL
    })
{
}

void BotAuctionOutcomeScript::OnBeforeAuctionHouseMgrSendAuctionSuccessfulMail(
    AuctionHouseMgr* /*auctionHouseMgr*/, AuctionEntry* auction, Player* owner, uint32& /*owner_accId*/,
    uint32& profit, bool& /*sendNotification*/, bool& /*updateAchievementCriteria*/, bool& sendMail)
{
    Player* bot = GetOnlineRandomBotOwner(owner);
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
    Player* bot = GetOnlineRandomBotOwner(owner);
    if (!bot)
        return;

    Item* item = auctionHouseMgr->GetAItem(auction->item_guid);
    if (!item)
        return;

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

void BotAuctionOutcomeScript::OnBeforeAuctionHouseMgrSendAuctionSalePendingMail(
    AuctionHouseMgr* /*auctionHouseMgr*/, AuctionEntry* /*auction*/, Player* owner, uint32& /*owner_accId*/,
    bool& sendMail)
{
    // Purely informational mail (no money, no items) sent to the owner on buyouts - for a bot that
    // never opens its mailbox this is pure mail-table growth, so drop it.
    if (GetOnlineRandomBotOwner(owner))
        sendMail = false;
}
