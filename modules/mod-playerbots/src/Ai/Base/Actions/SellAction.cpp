/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SellAction.h"

#include "Event.h"
#include "ItemUsageValue.h"
#include "ItemVisitors.h"
#include "LootAction.h"
#include "Playerbots.h"
#include "ItemPackets.h"

#include <unordered_map>

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
