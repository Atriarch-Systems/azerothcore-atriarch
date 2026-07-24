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
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <unordered_set>

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

    // Every reagent entry consumed by a tradeskill create-item recipe the given bot actually
    // knows - the crafting-loop exemption for the expired-return policy below (an enchanter's
    // expired dust listings come home instead of being vendored/destroyed). Copied verbatim from
    // the identical helper in AuctionBuyAction.cpp (Phase 4b, read-only reference): the bot
    // spell-map walk filtered exactly like CraftRandomItemAction::AcceptSpell
    // (CastCustomSpellAction.cpp). One full spell-map walk per expired bot auction - expiries are
    // sporadic (a few per bot per listing cycle), so this stays negligible.
    std::unordered_set<uint32> KnownRecipeReagents(Player* bot)
    {
        std::unordered_set<uint32> reagents;
        for (auto const& spellPair : bot->GetSpellMap())
        {
            if (spellPair.second->State == PLAYERSPELL_REMOVED || !spellPair.second->Active)
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellPair.first);
            if (!spellInfo)
                continue;

            if (spellInfo->Effects[EFFECT_0].Effect != SPELL_EFFECT_CREATE_ITEM ||
                !spellInfo->ReagentCount[EFFECT_0] || spellInfo->SchoolMask != 0)
                continue;

            for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            {
                if (spellInfo->Reagent[i] > 0)
                    reagents.insert(uint32(spellInfo->Reagent[i]));
            }
        }

        return reagents;
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

    ItemTemplate const* proto = item->GetTemplate();

    // Disposal policy for expired bot auctions - user-directed precedence (docs/bot-economy.md,
    // Phase 6e), evaluated EXACTLY in this order:
    //  1. Own-recipe reagent: if the OWNER bot's known recipes consume this entry, return it to
    //     bags regardless of anything else (crafting-loop exemption - an enchanter keeps its
    //     dusts for its own crafting instead of shredding them below).
    //  2. Quality <= RARE and vendorable (SellPrice > 0): "vendor from the mail" - credit the
    //     vendor price as gold and destroy the item. One listing attempt per item, ever: nobody
    //     bought it at auction, so it takes the vendor price instead of clogging bags/relists.
    //  3. Quality >= EPIC: too valuable to shred - return to bags for relisting.
    //  4. Unvendorable (SellPrice == 0; only quality <= RARE can reach here): destroy only if the
    //     entry still sits at/over the per-item flood cap (the pre-existing overpopulation sink),
    //     else return to bags.
    // Every "return to bags" branch keeps the existing bags-full -> mail fallback at the bottom.
    if (!KnownRecipeReagents(bot).count(proto->ItemId))
    {
        if (proto->Quality <= ITEM_QUALITY_RARE && proto->SellPrice > 0)
        {
            // Rule 2: vendor from the mail. Credit the full-stack vendor price and persist the
            // gold immediately in its own transaction (same crash-window-minimizing pattern as
            // the sold path above), then destroy the item exactly like the overpopulation sink
            // below: RemoveAItem(deleteItem=true) sets ITEM_REMOVED + saves, deleting it from
            // item_instance and the manager's map, so the caller's later plain
            // RemoveAItem(item_guid) is a safe no-op.
            uint32 const vendorValue = proto->SellPrice * auction->itemCount;
            bot->ModifyMoney(static_cast<int32>(vendorValue));

            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            bot->SaveGoldToDB(trans);
            auctionHouseMgr->RemoveAItem(auction->item_guid, true, &trans);
            CharacterDatabase.CommitTransaction(trans);

            sendMail = false;

            LOG_INFO("playerbots", "Bot {} vendored expired auction {}: item {} count {} for {} copper",
                bot->GetName(), auction->Id, auction->item_template, auction->itemCount, vendorValue);
            return;
        }

        if (proto->Quality < ITEM_QUALITY_EPIC)
        {
            // Rule 4 - overpopulation item sink for unvendorable materials: if the house already
            // carries at least the per-item cap (AiPlayerbot.MaxAuctionsPerItem) of this entry -
            // per the shared per-house entry-count cache StoreLootAction::AuctionItem maintains
            // (BotAuctionMarket, LootAction.h) - then nobody wanted this item at cap-level supply,
            // and returning it to bags would only make the bot relist it into the same glut
            // forever. Destroy it instead: SetState(ITEM_REMOVED) + save deletes it from
            // item_instance and drops it from the manager's map; the caller's later plain
            // RemoveAItem(item_guid) then finds nothing and is a safe no-op. True item
            // destruction, so log it at INFO for observability.
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
        }
        // Rule 3 (quality >= EPIC) and under-cap rule-4 items fall through to the
        // return-to-bags path below, exactly like rule-1 own-recipe reagents.
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
