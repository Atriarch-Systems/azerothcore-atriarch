/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTAUCTIONOUTCOMESCRIPT_H
#define PLAYERBOTS_BOTAUCTIONOUTCOMESCRIPT_H

#include "ScriptMgr.h"

// Closes the auction-outcome mail black hole for random bots (autonomous auction selling,
// AutoAuctionSellAction). Bots never read mail, and the core's old-mail sweeper
// (ObjectMgr::ReturnOrDeleteOldMails) skips logged-in receivers - random bots are ~always online -
// so auction-outcome mail piles up forever, and expired-auction mail older than 30 days is DELETED
// together with its items. Instead of mailing, this script resolves auction outcomes for random-bot
// owners directly at the source, via the core's pre-mail hooks (which expose bool& sendMail
// out-params exactly for this):
//
//  - SOLD:    credit the sale profit straight to the online bot (ModifyMoney + immediate gold save)
//             and suppress the owner mail.
//  - EXPIRED: resolved per the user-directed disposal policy (docs/bot-economy.md, Phase 6e),
//             evaluated in exactly this precedence order:
//              1. the owner bot's own known recipes consume the entry -> return to bags
//                 (crafting-loop exemption, checked before everything else);
//              2. quality <= RARE and vendorable (SellPrice > 0) -> "vendored from the mail":
//                 the vendor price is credited as gold and the item destroyed - each item gets
//                 one listing attempt, ever;
//              3. quality >= EPIC -> return to bags for relisting;
//              4. unvendorable (SellPrice == 0) -> destroyed only when the entry still sits
//                 at/above the AiPlayerbot.MaxAuctionsPerItem active-listing cap (the
//                 overpopulation item sink, per the shared BotAuctionMarket cache maintained by
//                 StoreLootAction::AuctionItem), else returned to bags.
//             "Return to bags" uses the same CanStoreItem/MoveItemToInventory sequence
//             HandleMailTakeItem uses and suppresses the owner mail; if the bags are full, the
//             normal mail path runs as today - bounded leakage beats item destruction. Every
//             destruction/vendoring is LOG_INFO'd for observability.
//  - WON:     put the won item straight into the online random-bot bidder's bags (same sequence as
//             EXPIRED) and suppress the winner mail; bags-full/offline falls back to mail as
//             today. This is the prerequisite for bot AH buying - won items must reach bags, not
//             an unread mailbox.
//  - SALE PENDING: the buyout path (HandleAuctionPlaceBid) additionally sends the owner an
//             informational "sale pending" mail carrying no money or items - suppressed outright
//             for random-bot owners, it would only ever accumulate.
//
// If the owning bot is offline (rare - random bots are ~always online here) or is not a random bot
// (real players, personal bots), the hooks do nothing and stock mail behavior is preserved.
// The whole script is gated by AiPlayerbot.AuctionOutcomeDirect (default: enabled).
class BotAuctionOutcomeScript : public AuctionHouseScript
{
public:
    BotAuctionOutcomeScript();

    void OnBeforeAuctionHouseMgrSendAuctionSuccessfulMail(AuctionHouseMgr* auctionHouseMgr, AuctionEntry* auction,
        Player* owner, uint32& owner_accId, uint32& profit, bool& sendNotification, bool& updateAchievementCriteria,
        bool& sendMail) override;

    void OnBeforeAuctionHouseMgrSendAuctionExpiredMail(AuctionHouseMgr* auctionHouseMgr, AuctionEntry* auction,
        Player* owner, uint32& owner_accId, bool& sendNotification, bool& sendMail) override;

    void OnBeforeAuctionHouseMgrSendAuctionWonMail(AuctionHouseMgr* auctionHouseMgr, AuctionEntry* auction,
        Player* bidder, uint32& bidder_accId, bool& sendNotification, bool& updateAchievementCriteria,
        bool& sendMail) override;

    void OnBeforeAuctionHouseMgrSendAuctionSalePendingMail(AuctionHouseMgr* auctionHouseMgr, AuctionEntry* auction,
        Player* owner, uint32& owner_accId, bool& sendMail) override;
};

#endif
