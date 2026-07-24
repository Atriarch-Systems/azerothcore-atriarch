/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AuctionBuyAction.h"

#include "AuctionHouseMgr.h"
#include "BudgetValues.h"
#include "CharacterCache.h"
#include "Config.h"
#include "Event.h"
#include "ItemUsageValue.h"
#include "Opcodes.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // A buyout the bot decided to attempt during the scan pass. Only the id and the price seen at
    // scan time are kept - AuctionEntry pointers are never held across the purchase (the buyout
    // path frees the entry), each candidate is re-resolved by id right before the packet is sent.
    struct AuctionBuyCandidate
    {
        uint32 auctionId = 0;
        uint32 buyout = 0;
    };

    // Resume cursor for the capped scan pass (AiPlayerbot.AuctionBuyScanCap), keyed by houseId:
    // the last auction id the previous pass (by ANY bot) processed in that house. The house's
    // auction map is a std::map ordered by auction id, so successive passes resume at
    // upper_bound(cursor) and wrap to the start when the end is reached - together the passes
    // sweep the whole house in slices instead of every pass re-walking the same first entries.
    // Mutex-guarded because buy passes run from map-update contexts on parallel map threads.
    std::mutex auctionBuyCursorLock;
    std::unordered_map<uint32, uint32> auctionBuyCursorByHouse;

    // Every reagent entry used by a tradeskill create-item recipe the bot actually knows. This is
    // the cheapest existing enumeration: the same bot spell-map walk the master-ordered "craft"
    // command uses to compute reagents (SetCraftAction::Execute, read-only reference), filtered
    // exactly like CraftRandomItemAction::AcceptSpell (CastCustomSpellAction.cpp) so we only buy
    // materials the autonomous craft pass (Phase 4a) can actually consume.
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

bool AuctionBuyAction::isUseful()
{
    // Unattended RANDOM bots only, for the same reasons as AutoAuctionSellAction::isUseful()
    // (SellAction.cpp): a personal bot whose owner is merely offline also has no active master,
    // and must not spend its owner's gold behind their back.
    static bool const autoAuctionBuy = sConfigMgr->GetOption<bool>("AiPlayerbot.AutoAuctionBuy", true);
    return autoAuctionBuy && !botAI->HasActivePlayerMaster() && sRandomPlayerbotMgr.IsRandomBot(bot);
}

bool AuctionBuyAction::Execute(Event /*event*/)
{
    // Full-evaluation cap per pass, shared between the gear-upgrade pass (ItemUsageValue calls,
    // ~0.1ms each) and the reagent pass, so one bot-pass stays bounded no matter how big the
    // house is (docs/bot-economy.md, Phase 3).
    static uint32 const MAX_FULL_EVALUATIONS_PER_PASS = 25;

    static uint32 const maxPerPass = sConfigMgr->GetOption<uint32>("AiPlayerbot.AuctionBuyMaxPerPass", 1);
    if (!maxPerPass)
        return false;

    // Live auctioneer NPC in interact range - buying keys off its faction exactly like the real
    // CMSG_AUCTION_PLACE_BID handler does (same scan as StoreLootAction::AuctionItem,
    // LootAction.cpp).
    GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
    Creature* auctioneer = nullptr;
    for (ObjectGuid const guid : npcs)
    {
        if (Creature* creature = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_AUCTIONEER))
        {
            auctioneer = creature;
            break;
        }
    }

    if (!auctioneer)
        return false;

    // GetAuctionsMap() takes the faction TEMPLATE id and folds in the two-sided-interaction
    // config itself (returns the Neutral house when that's set) - see the API notes on
    // StoreLootAction::AuctionItem. The DBC entry is fetched the same way purely for its houseId,
    // which keys the scan resume cursor below (consistent under two-sided folding, unlike the raw
    // faction id).
    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
    AuctionHouseEntry const* ahEntry =
        AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(auctioneer->GetFaction());
    if (!auctionHouse || !ahEntry)
        return false;

    // Budgets: the existing budget system already reserves repair/consumable/travel money, so
    // "free money for X" is what the bot can genuinely afford to spend on X (idiom from
    // BuyAction.cpp). Tracked locally and decremented per purchase within this pass.
    uint32 gearBudget = AI_VALUE2(uint32, "free money for", (uint32)NeedMoneyFor::gear);

    // Phase 4b: crafter bots also shop for reagents their known recipes consume, on a separate
    // (smaller) budget. Non-crafters get an empty set and skip the trade-goods branch entirely.
    std::unordered_set<uint32> const recipeReagents = KnownRecipeReagents(bot);
    uint32 reagentBudget =
        recipeReagents.empty() ? 0 : AI_VALUE2(uint32, "free money for", (uint32)NeedMoneyFor::tradeskill);

    if (!gearBudget && !reagentBudget)
        return false;

    uint32 const accountId = bot->GetSession()->GetAccountId();

    std::vector<AuctionBuyCandidate> gearCandidates;
    std::vector<AuctionBuyCandidate> reagentCandidates;
    uint32 evaluations = 0;

    // Total-iteration cap for the scan pass: even the "cheap" pre-filters (a GetItemTemplate per
    // entry) add up when the house holds 10k+ auctions and hundreds of bots run buy passes. Each
    // pass walks at most scanCap entries, resuming after the shared per-house cursor and wrapping
    // to the start when the end is reached. 0 = uncapped full-house scan (kill switch, pre-cap
    // behavior).
    static uint32 const scanCap = sConfigMgr->GetOption<uint32>("AiPlayerbot.AuctionBuyScanCap", 500);

    uint32 cursor = 0;
    if (scanCap)
    {
        std::lock_guard<std::mutex> lock(auctionBuyCursorLock);
        cursor = auctionBuyCursorByHouse[ahEntry->houseId];
    }

    // Scan pass: iterate and decide synchronously, buy later - the buyout path removes entries
    // from this very map, so no purchases may happen while iterating it.
    auto const& auctions = auctionHouse->GetAuctions();
    auto auctionItr = scanCap ? auctions.upper_bound(cursor) : auctions.begin();
    uint32 lastProcessedId = cursor;
    uint32 iterations = 0;
    bool wrapped = false;
    while (true)
    {
        if (auctionItr == auctions.end())
        {
            // Wrap to the start once so a cursor near the end of the map still yields a full
            // slice. A second arrival at end() - or any arrival when uncapped or starting from
            // the beginning - means the whole house was covered.
            if (!scanCap || wrapped || cursor == 0)
                break;

            wrapped = true;
            auctionItr = auctions.begin();
            continue;
        }

        // After wrapping, stop once we're back past the starting point - full circle.
        if (wrapped && auctionItr->first > cursor)
            break;

        if (scanCap && iterations >= scanCap)
            break;

        if (evaluations >= MAX_FULL_EVALUATIONS_PER_PASS)
            break;

        AuctionEntry const* auction = auctionItr->second;
        lastProcessedId = auctionItr->first;
        ++auctionItr;
        ++iterations;

        // Cheap pre-filters first, most selective and cheapest up front: buyout-only...
        if (!auction->buyout)
            continue;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(auction->item_template);
        if (!proto)
            continue;

        // ...only item classes either pass cares about...
        bool const isGear = proto->Class == ITEM_CLASS_ARMOR || proto->Class == ITEM_CLASS_WEAPON;
        bool const isKnownReagent =
            proto->Class == ITEM_CLASS_TRADE_GOODS && recipeReagents.find(auction->item_template) != recipeReagents.end();
        if (!isGear && !isKnownReagent)
            continue;

        // ...usable and affordable (per-pass budget, not raw money)...
        if (isGear && (proto->RequiredLevel > bot->GetLevel() || auction->buyout > gearBudget))
            continue;

        if (isKnownReagent && auction->buyout > reagentBudget)
            continue;

        // ...never own or already-bid auctions, mirroring HandleAuctionPlaceBid's own checks
        // (AuctionHouseHandler.cpp ~471-485). Deliberately STRICTER than the handler on the
        // same-account rule: the handler skips the account lookup when the owner is online
        // (impossible for a real player to have two characters online), but random-bot accounts
        // keep many characters online at once, so the shortcut doesn't hold here - always check.
        if (auction->owner == bot->GetGUID() || auction->bidder == bot->GetGUID())
            continue;

        if (sCharacterCache->GetCharacterAccountIdByGuid(auction->owner) == accountId)
            continue;

        // Full evaluation - both branches count against the shared per-pass cap.
        ++evaluations;

        if (isGear)
        {
            // Genuine upgrades only: EQUIP (empty slot) or REPLACE (better than current).
            ItemUsage const usage = AI_VALUE2(ItemUsage, "item usage", auction->item_template);
            if (usage != ITEM_USAGE_EQUIP && usage != ITEM_USAGE_REPLACE)
                continue;

            gearCandidates.push_back({auction->Id, auction->buyout});
        }
        else
        {
            // Cheap-only reagent heuristic (v1, no market strategy): per-unit buyout must not
            // exceed 1.5x the template's vendor BuyPrice; templates without a vendor price
            // (BuyPrice 0) fall back to 6x SellPrice (~1.5x the standard 4x vendor markup), and
            // items with neither price are skipped rather than bought at any price.
            uint32 const stackCount = std::max<uint32>(auction->itemCount, 1);
            uint64 const perUnitCap =
                proto->BuyPrice ? uint64(proto->BuyPrice) + proto->BuyPrice / 2 : uint64(proto->SellPrice) * 6;
            if (!perUnitCap || uint64(auction->buyout) > perUnitCap * stackCount)
                continue;

            reagentCandidates.push_back({auction->Id, auction->buyout});
        }
    }

    // Publish the resume point for the next pass (by any bot) over this house.
    if (scanCap)
    {
        std::lock_guard<std::mutex> lock(auctionBuyCursorLock);
        auctionBuyCursorByHouse[ahEntry->houseId] = lastProcessedId;
    }

    if (gearCandidates.empty() && reagentCandidates.empty())
        return false;

    uint32 bought = 0;

    auto tryBuyout = [&](AuctionBuyCandidate const& candidate, uint32& budget) -> void
    {
        // Re-resolve by auction id right before purchase - another buyer (or the house update)
        // may have removed or outbid it since the scan above, and the entry pointer from the scan
        // must never be trusted this late.
        AuctionEntry const* auction = auctionHouse->GetAuction(candidate.auctionId);
        if (!auction || !auction->buyout || auction->buyout != candidate.buyout)
            return;

        if (auction->owner == bot->GetGUID() || auction->bidder == bot->GetGUID())
            return;

        uint32 const price = auction->buyout;
        if (price > budget || !bot->HasEnoughMoney(price))
            return;

        // Capture everything needed after the call NOW: a successful buyout deletes the entry.
        uint32 const itemEntry = auction->item_template;
        uint32 const itemCount = auction->itemCount;

        // The real packet, through the real session handler - field order verified against
        // WorldSession::HandleAuctionPlaceBid (AuctionHouseHandler.cpp ~425-431): auctioneer
        // ObjectGuid, then uint32 auctionId, then uint32 price. Direct handler call, same
        // convention as SellAction::Sell's CMSG_SELL_ITEM (SellAction.cpp ~139-144); price ==
        // buyout selects the handler's buyout branch, which also routes the won item through the
        // hooked outcome path (BotAuctionOutcomeScript) into the bot's bags.
        WorldPacket packet(CMSG_AUCTION_PLACE_BID, 8 + 4 + 4);
        packet << auctioneer->GetGUID();
        packet << candidate.auctionId;
        packet << price;
        bot->GetSession()->HandleAuctionPlaceBid(packet);

        // The handler returns void and reports failures via a client packet; a successful buyout
        // is observable as the auction vanishing from the house map.
        if (auctionHouse->GetAuction(candidate.auctionId))
            return;

        budget = budget > price ? budget - price : 0;
        ++bought;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
        LOG_INFO("playerbots", "Playerbot {} bought out auction {} ({} x{}) for {} copper",
                 bot->GetName(), candidate.auctionId, proto ? proto->Name1 : "?", itemCount, price);
    };

    // Purchase pass: gear upgrades first, then reagents, sharing the MaxPerPass cap.
    for (AuctionBuyCandidate const& candidate : gearCandidates)
    {
        if (bought >= maxPerPass)
            break;

        tryBuyout(candidate, gearBudget);
    }

    for (AuctionBuyCandidate const& candidate : reagentCandidates)
    {
        if (bought >= maxPerPass)
            break;

        tryBuyout(candidate, reagentBudget);
    }

    return bought > 0;
}
