/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SellAction.h"

#include "AuctionHouseMgr.h"
#include "Config.h"
#include "Event.h"
#include "ItemUsageValue.h"
#include "ItemVisitors.h"
#include "Log.h"
#include "LootAction.h"
#include "Playerbots.h"
#include "ItemPackets.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <unordered_map>
#include <unordered_set>

class SellItemsVisitor : public IterateItemsVisitor
{
public:
    SellItemsVisitor(SellAction* action) : IterateItemsVisitor(), action(action) {}

    bool Visit(Item* item) override
    {
        action->Sell(item);
        return true;
    }

private:
    SellAction* action;
};

class SellGrayItemsVisitor : public SellItemsVisitor
{
public:
    SellGrayItemsVisitor(SellAction* action) : SellItemsVisitor(action) {}

    bool Visit(Item* item) override
    {
        if (item->GetTemplate()->Quality != ITEM_QUALITY_POOR)
            return true;

        return SellItemsVisitor::Visit(item);
    }
};

class SellVendorItemsVisitor : public SellItemsVisitor
{
public:
    SellVendorItemsVisitor(SellAction* action, PlayerbotAI* botAI) : SellItemsVisitor(action), botAI(botAI) {}

    PlayerbotAI* botAI;

    bool Visit(Item* item) override
    {
        AiObjectContext* context = botAI->GetAiObjectContext();
        ItemUsage usage = context->GetValue<ItemUsage>("item usage", item->GetEntry())->Get();
        if (usage != ITEM_USAGE_VENDOR && usage != ITEM_USAGE_AH)
            return true;

        // AH-tagged BoE items are worth more sold on the auction house than dumped on a vendor
        // (ItemUsageValue::Calculate() only tags sellable, non-soulbound, Normal-quality+ BoE items
        // this way in the first place) - route those through the real auction path instead of the
        // vendor-sell packet below.
        if (usage == ITEM_USAGE_AH)
        {
            if (StoreLootAction::AuctionItem(item, botAI))
                return true;

            // No auctioneer in range (or the listing failed / hit the per-bot cap): KEEP the item
            // in the bags so the periodic auto-auction pass gets a chance at it, instead of
            // shredding its AH value on a vendor. Only when the bags are nearly full (same >= 90%
            // threshold SmartDestroyItemAction uses) is the vendor fallback allowed, so bots don't
            // clog themselves hoarding unlistable stock.
            if (context->GetValue<uint8>("bag space")->Get() < 90)
                return true;
        }

        return SellItemsVisitor::Visit(item);
    }
};

// Collects every unvendorable (SellPrice == 0) trade-goods stack in the bags - the candidate set
// for AutoVendorSellAction's over-cap destroy pass (rule (c) below); the over-cap and own-recipe
// filters run on the collected list, not here, so the expensive checks only happen at all when
// such items exist.
class CollectUnvendorableTradeGoodsVisitor : public IterateItemsVisitor
{
public:
    CollectUnvendorableTradeGoodsVisitor() : IterateItemsVisitor() {}

    bool Visit(Item* item) override
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (proto->Class == ITEM_CLASS_TRADE_GOODS && !proto->SellPrice)
            items.push_back(item);

        return true;
    }

    std::vector<Item*> items;
};

namespace
{
    // Every reagent entry consumed by a tradeskill create-item recipe the bot actually knows -
    // the crafting-loop exemption for the destroy pass (an enchanter keeps its dusts). Copied
    // verbatim from the identical helper in AuctionBuyAction.cpp (Phase 4b, read-only reference):
    // the bot spell-map walk filtered exactly like CraftRandomItemAction::AcceptSpell
    // (CastCustomSpellAction.cpp), so the set matches what the autonomous craft pass can consume.
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

bool SellAction::Execute(Event event)
{
    std::string const text = event.getParam();
    if (text == "gray" || text == "*")
    {
        SellGrayItemsVisitor visitor(this);
        IterateItems(&visitor);
        return true;
    }

    if (text == "vendor")
    {
        SellVendorItemsVisitor visitor(this, botAI);
        IterateItems(&visitor);
        return true;
    }

    if (text != "")
    {
        std::vector<Item*> items = parseItems(text, ITERATE_ITEMS_IN_BAGS);
        for (Item* item : items)
        {
            Sell(item);
        }
        return true;
    }

    botAI->TellError("Usage: s gray/*/vendor/[item link]");
    return false;
}

void SellAction::Sell(FindItemVisitor* visitor)
{
    IterateItems(visitor);
    std::vector<Item*> items = visitor->GetResult();
    for (Item* item : items)
    {
        Sell(item);
    }
}

void SellAction::Sell(Item* item)
{
    std::ostringstream out;

    GuidVector vendors = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest npcs")->Get();

    for (ObjectGuid const vendorguid : vendors)
    {
        Creature* pCreature = bot->GetNPCIfCanInteractWith(vendorguid, UNIT_NPC_FLAG_VENDOR);
        if (!pCreature)
            continue;

        ObjectGuid itemguid = item->GetGUID();
        uint32 count = item->GetCount();

        uint32 botMoney = bot->GetMoney();

        WorldPacket p(CMSG_SELL_ITEM);
        p << vendorguid << itemguid << count;

        WorldPackets::Item::SellItem nicePacket(std::move(p));
        nicePacket.Read();
        bot->GetSession()->HandleSellItemOpcode(nicePacket);

        if (botAI->HasCheat(BotCheatMask::gold))
        {
            bot->SetMoney(botMoney);
        }

        out << "Selling " << chat->FormatItem(item->GetTemplate());
        botAI->TellMaster(out);

        bot->PlayDistanceSound(120);
        break;
    }
}

bool AutoAuctionSellAction::isUseful()
{
    // Unattended RANDOM bots only - a bot with a real, currently-active player master shouldn't
    // silently list that player's loot on the AH behind their back, and !HasActivePlayerMaster()
    // alone is not enough: a personal (non-random) bot whose owner is merely OFFLINE also has no
    // active master, and would liquidate its owner's items the moment they log out. Real players
    // still have full manual control via the "s vendor"/"s *"/"s [item link]" chat commands above.
    return sPlayerbotAIConfig.autoAuctionSell && !botAI->HasActivePlayerMaster() &&
           sRandomPlayerbotMgr.IsRandomBot(bot);
}

bool AutoAuctionSellAction::Execute(Event /*event*/)
{
    // A handful per check is plenty - this trigger already only fires every few minutes
    // (AutoAuctionSellTrigger), and each successful listing costs the bot an AH deposit, so there's
    // no benefit to draining an entire bag of AH-tagged loot in one pass.
    static uint32 const MAX_AUTO_AUCTION_ITEMS_PER_CHECK = 3;

    std::vector<Item*> items =
        AI_VALUE2(std::vector<Item*>, "inventory items", "usage " + std::to_string(ITEM_USAGE_AH));

    // For stackable entries, find the largest stack per entry first - each entry gets at most one
    // listing per pass, and a fat stack is a far better use of a listing slot (and its deposit)
    // than whatever tiny partial stack happens to sit first in the bags. (Cross-stack
    // consolidation - merging partial stacks into one listing via a cloned item - is deliberately
    // NOT done here; deferred until the simple largest-stack rule proves insufficient.)
    std::unordered_map<uint32, Item*> largestStackByEntry;
    for (Item* item : items)
    {
        if (item->GetMaxStackCount() <= 1)
            continue;

        Item*& largest = largestStackByEntry[item->GetEntry()];
        if (!largest || item->GetCount() > largest->GetCount())
            largest = item;
    }

    uint8 const bagSpace = AI_VALUE(uint8, "bag space");

    uint32 listed = 0;
    for (Item* item : items)
    {
        if (listed >= MAX_AUTO_AUCTION_ITEMS_PER_CHECK)
            break;

        // Non-stackables can't do better than being listed as-is, in bag order, as before.
        if (item->GetMaxStackCount() > 1)
        {
            // Only the entry's largest stack represents it; skip the smaller duplicates.
            Item* largest = largestStackByEntry[item->GetEntry()];
            if (item != largest)
                continue;

            // Trade goods trickle in from gathering and will keep growing toward a full stack -
            // don't waste a deposit listing a half-empty one yet. Override when the bags are
            // nearly full (same >= 90% threshold the vendor fallback above uses): freeing the
            // slot now beats hoarding toward a fuller stack the bot has no room to collect.
            ItemTemplate const* proto = item->GetTemplate();
            if (proto->Class == ITEM_CLASS_TRADE_GOODS && item->GetCount() < proto->GetMaxStackSize() / 2 &&
                bagSpace < 90)
                continue;
        }

        if (StoreLootAction::AuctionItem(item, botAI))
            ++listed;
    }

    return listed > 0;
}

bool AutoVendorSellAction::isUseful()
{
    // Unattended RANDOM bots only, for exactly the reasons documented on
    // AutoAuctionSellAction::isUseful() above - a personal bot whose owner is merely offline must
    // not liquidate (or destroy!) its owner's items behind their back. Inline config read cached
    // in a function-local static, same convention as the other new economy keys (EconomyTriggers.cpp).
    static bool const autoVendorSell = sConfigMgr->GetOption<bool>("AiPlayerbot.AutoVendorSell", true);
    return autoVendorSell && !botAI->HasActivePlayerMaster() && sRandomPlayerbotMgr.IsRandomBot(bot);
}

bool AutoVendorSellAction::Execute(Event /*event*/)
{
    // Bound one pass's work - vendor junk trickles in slowly and the trigger fires every 5
    // minutes, so a small cap per pass drains any realistic backlog while keeping a single AI
    // tick's cost fixed. Shared between the two sell rules and the destroy rule below.
    static uint32 const MAX_AUTO_VENDOR_ITEMS_PER_CHECK = 10;

    // Selling goes through the real CMSG_SELL_ITEM session handler via SellAction::Sell(), which
    // needs a live VENDOR-flagged NPC in interact range - same "nearest npcs" +
    // GetNPCIfCanInteractWith(UNIT_NPC_FLAG_VENDOR) scan Sell() itself does. Re-checked here even
    // though AutoVendorSellTrigger already scanned, because the trigger result can be minutes old
    // and the destroy rule below must never run when the sell rules can't.
    GuidVector vendors = AI_VALUE(GuidVector, "nearest npcs");
    bool vendorInRange = false;
    for (ObjectGuid const vendorguid : vendors)
    {
        if (bot->GetNPCIfCanInteractWith(vendorguid, UNIT_NPC_FLAG_VENDOR))
        {
            vendorInRange = true;
            break;
        }
    }

    if (!vendorInRange)
        return false;

    // Flood-cap context for rules (b) and (c). There's no auctioneer here to key the house off
    // (this is a vendor pass), so use the bot's OWN faction template - core's
    // GetAuctionHouseEntryFromFactionTemplate resolves it through the same FACTION_MASK/two-sided
    // -config logic the listing side uses, so the houseId matches what StoreLootAction::AuctionItem
    // cached for this bot's listings. GetCachedAuctionCountForEntry returns 0 for unknown/stale
    // caches, so "no data" safely reads as "not overpopulated" (items are kept, not dumped).
    uint32 const maxAuctionsPerItem = BotAuctionMarket::GetMaxAuctionsPerItem();
    uint32 houseId = 0;
    if (maxAuctionsPerItem)
    {
        if (AuctionHouseEntry const* ahEntry =
            AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(bot->GetFaction()))
            houseId = ahEntry->houseId;
    }

    auto const entryOverCap = [&](uint32 itemEntry) -> bool
    {
        return maxAuctionsPerItem && houseId &&
               BotAuctionMarket::GetCachedAuctionCountForEntry(houseId, itemEntry) >= maxAuctionsPerItem;
    };

    uint32 processed = 0;

    // Rule (a) - the missing junk outflow: everything ItemUsageValue tagged VENDOR (greys, unworn
    // soulbound, unlistable BuyPrice-0 whites).
    std::vector<Item*> items =
        AI_VALUE2(std::vector<Item*>, "inventory items", "usage " + std::to_string(ITEM_USAGE_VENDOR));
    for (Item* item : items)
    {
        if (processed >= MAX_AUTO_VENDOR_ITEMS_PER_CHECK)
            return true;

        Sell(item);
        ++processed;
    }

    // Rules (b) and (c) only exist while the per-item flood cap is enabled and the bot's house is
    // resolvable - without a cap there's no such thing as an over-cap entry.
    if (maxAuctionsPerItem && houseId)
    {
        // Rule (b): AH-tagged items whose entry already sits at/over the cap - the house doesn't
        // need more of these, so vendoring beats waiting forever for a listing slot.
        items = AI_VALUE2(std::vector<Item*>, "inventory items", "usage " + std::to_string(ITEM_USAGE_AH));
        for (Item* item : items)
        {
            if (processed >= MAX_AUTO_VENDOR_ITEMS_PER_CHECK)
                return true;

            if (!entryOverCap(item->GetEntry()))
                continue;

            Sell(item);
            ++processed;
        }

        // Rule (c): unvendorable (SellPrice == 0) trade goods over the cap can go NOWHERE - not
        // vendor, not AH - so destroy them... unless the bot's own known recipes consume the entry
        // (crafting-loop exemption). Filter to over-cap candidates FIRST so the recipe enumeration
        // (a full spell-map walk) runs at most once per pass, and only when there's actually
        // something to destroy.
        CollectUnvendorableTradeGoodsVisitor visitor;
        IterateItems(&visitor);

        std::vector<Item*> destroyCandidates;
        for (Item* item : visitor.items)
        {
            if (entryOverCap(item->GetEntry()))
                destroyCandidates.push_back(item);
        }

        if (!destroyCandidates.empty())
        {
            std::unordered_set<uint32> const ownReagents = KnownRecipeReagents(bot);
            for (Item* item : destroyCandidates)
            {
                if (processed >= MAX_AUTO_VENDOR_ITEMS_PER_CHECK)
                    return true;

                if (ownReagents.find(item->GetEntry()) != ownReagents.end())
                    continue;

                // Like the expired-auction overpopulation sink (BotAuctionOutcomeScript.cpp), this
                // is true item destruction - LOG_INFO for observability.
                LOG_INFO("playerbots", "Playerbot {} destroyed {} x{} (entry {}): unvendorable trade goods over the per-item auction cap {}",
                    bot->GetName(), item->GetTemplate()->Name1, item->GetCount(), item->GetEntry(), maxAuctionsPerItem);

                bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
                ++processed;
            }
        }
    }

    return processed > 0;
}
