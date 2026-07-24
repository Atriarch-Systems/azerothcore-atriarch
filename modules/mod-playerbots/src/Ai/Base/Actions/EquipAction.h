/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_EQUIPACTION_H
#define PLAYERBOTS_EQUIPACTION_H

#include "ChatHelper.h"
#include "InventoryAction.h"
#include "Item.h"

class FindItemVisitor;
class Item;
class PlayerbotAI;

class EquipAction : public InventoryAction
{
public:
    EquipAction(PlayerbotAI* botAI, std::string const name = "equip") : InventoryAction(botAI, name) {}

    bool Execute(Event event) override;
    void EquipItems(ItemIds ids);
    ItemIds SelectInventoryItemsToEquip();

private:
    void EquipItem(FindItemVisitor* visitor);
    uint8 GetSmallestBagSlot();
    void EquipItem(Item* item);

    // Post-equip verification + honest status whisper. The equip opcodes report failure only via
    // SendEquipError packets a headless bot session never reads, so the old unconditional
    // "Equipping X" message lied whenever the swap silently failed (most commonly
    // EQUIP_ERR_NOT_IN_COMBAT: armor cannot be equipped mid-fight, and loot-roll wins arrive mid-
    // dungeon-pull). Checks whether dstSlot now actually holds the item and phrases accordingly.
    void TellEquipResult(Item* item, uint8 dstSlot, char const* suffix = nullptr);
};

class EquipUpgradesPacketAction : public EquipAction
{
public:
    explicit EquipUpgradesPacketAction(PlayerbotAI* botAI, std::string const name = "equip upgrades packet action") : EquipAction(botAI, name) {}

    bool Execute(Event event) override;
};

class EquipUpgradeAction : public EquipAction
{
public:
    explicit EquipUpgradeAction(PlayerbotAI* botAI, std::string const name = "equip upgrade") : EquipAction(botAI, name) {}

    bool Execute(Event event) override;
};

#endif
