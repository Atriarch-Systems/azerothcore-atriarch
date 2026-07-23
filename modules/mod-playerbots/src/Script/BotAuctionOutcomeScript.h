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
//  - EXPIRED: put the unsold item straight back into the online bot's bags (the same
//             CanStoreItem/MoveItemToInventory sequence HandleMailTakeItem uses) and suppress the
//             owner mail. If the bags are full, the normal mail path runs as today - bounded
//             leakage beats item destruction. EXCEPTION - the overpopulation item sink: when the
//             expiring item's entry already sits at/above the AiPlayerbot.MaxAuctionsPerItem
//             active-listing cap (per the shared BotAuctionMarket cache maintained by
//             StoreLootAction::AuctionItem), the item is destroyed outright instead of returned.
//             This is the bot economy's only true item sink, keeping gathered-mat populations
//             bounded; each destruction is LOG_INFO'd for observability.
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
