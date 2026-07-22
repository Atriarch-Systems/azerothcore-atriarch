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
        // vendor-sell packet below. Falls through to the normal vendor sell if no auctioneer is in
        // range or the listing otherwise fails (AuctionItem() returns false without side effects),
        // so "s vendor"/"s *" still get rid of the item one way or another.
        if (usage == ITEM_USAGE_AH && StoreLootAction::AuctionItem(item->GetEntry(), botAI))
            return true;

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
    // Unattended/random bots only - a bot with a real, currently-active player master shouldn't
    // silently list that player's loot on the AH behind their back. Those bots still have full
    // manual control via the "s vendor"/"s *"/"s [item link]" chat commands above either way.
    return sPlayerbotAIConfig.autoAuctionSell && !botAI->HasActivePlayerMaster();
}

bool AutoAuctionSellAction::Execute(Event /*event*/)
{
    // A handful per check is plenty - this trigger already only fires every few minutes
    // (AutoAuctionSellTrigger), and each successful listing costs the bot an AH deposit, so there's
    // no benefit to draining an entire bag of AH-tagged loot in one pass.
    static uint32 const MAX_AUTO_AUCTION_ITEMS_PER_CHECK = 3;

    std::vector<Item*> items =
        AI_VALUE2(std::vector<Item*>, "inventory items", "usage " + std::to_string(ITEM_USAGE_AH));

    uint32 listed = 0;
    for (Item* item : items)
    {
        if (listed >= MAX_AUTO_AUCTION_ITEMS_PER_CHECK)
            break;

        if (StoreLootAction::AuctionItem(item->GetEntry(), botAI))
            ++listed;
    }

    return listed > 0;
}
