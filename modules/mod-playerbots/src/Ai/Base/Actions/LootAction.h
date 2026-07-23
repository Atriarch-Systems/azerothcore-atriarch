/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LOOTACTION_H
#define PLAYERBOTS_LOOTACTION_H

#include "InventoryAction.h"
#include "MovementActions.h"

class GameObject;
class Item;
class LootObject;
class PlayerbotAI;
class SpellInfo;

// Shared view of the auction-listing flood caps maintained by StoreLootAction::AuctionItem()
// (LootAction.cpp). Exposed as free functions so BotAuctionOutcomeScript can consult the same
// per-house per-entry active-listing cache when deciding whether an expiring auction of an
// overpopulated item should be destroyed instead of returned (the economy's item sink).
// Both functions are safe to call from any thread (the cache is mutex-guarded internally).
namespace BotAuctionMarket
{
    // AiPlayerbot.MaxAuctionsPerItem: per-house cap on simultaneously active listings of one item
    // entry. 0 disables the cap (and the expired-overpopulation item sink with it).
    uint32 GetMaxAuctionsPerItem();

    // Cached count of active auctions of itemEntry in house houseId, as of the last house scan
    // (<= 3 minutes old). Returns 0 when no bot has listed in that house recently enough for a
    // live cache to exist - callers should treat that as "not overpopulated".
    uint32 GetCachedAuctionCountForEntry(uint32 houseId, uint32 itemEntry);
}

class LootAction : public MovementAction
{
public:
    LootAction(PlayerbotAI* botAI) : MovementAction(botAI, "loot") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

class OpenLootAction : public MovementAction
{
public:
    OpenLootAction(PlayerbotAI* botAI) : MovementAction(botAI, "open loot") {}

    bool Execute(Event event) override;

private:
    bool DoLoot(LootObject& lootObject);
    uint32 GetOpeningSpell(LootObject& lootObject);
    uint32 GetOpeningSpell(LootObject& lootObject, GameObject* go);
    bool CanOpenLock(LootObject& lootObject, SpellInfo const* spellInfo, GameObject* go);
    bool CanOpenLock(uint32 skillId, uint32 reqSkillValue);
};

class StoreLootAction : public InventoryAction
{
public:
    StoreLootAction(PlayerbotAI* botAI) : InventoryAction(botAI, "store loot") {}

    bool Execute(Event event) override;
    static bool IsLootAllowed(uint32 itemid, PlayerbotAI* botAI);

    // Lists a specific item stack on the auction house through a nearby auctioneer NPC. Returns
    // false (no-op, no side effects) if the item isn't listable, there's no auctioneer in range,
    // a flood cap is hit, or the bot can't cover the deposit. Takes the exact Item* (not an entry)
    // so callers control WHICH stack gets listed - GetItemByEntry() resolution used to grab the
    // first match, which could be a tiny partial stack. Static + takes botAI explicitly so callers
    // outside this class (SellAction.cpp, the autonomous AutoAuctionSellAction) can invoke it,
    // matching the IsLootAllowed() convention above.
    static bool AuctionItem(Item* item, PlayerbotAI* botAI);

private:
    static uint32 RoundPrice(double price);
};

class ReleaseLootAction : public InventoryAction
{
public:
    ReleaseLootAction(PlayerbotAI* botAI) : InventoryAction(botAI, "release loot") {}

    bool Execute(Event event) override;
};

#endif
