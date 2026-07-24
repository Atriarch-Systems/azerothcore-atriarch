/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "LootAction.h"

#include "AuctionHouseMgr.h"
#include "ChatHelper.h"
#include "Config.h"
#include "Event.h"
#include "GameTime.h"
#include "GuildMgr.h"
#include "GuildTaskMgr.h"
#include "ItemUsageValue.h"
#include "LootObjectStack.h"
#include "LootStrategyValue.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "GuildMgr.h"
#include "BroadcastHelper.h"
#include "World.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

bool LootAction::Execute(Event /*event*/)
{
    if (!AI_VALUE(bool, "has available loot"))
        return false;

    LootObject prevLoot = AI_VALUE(LootObject, "loot target");
    LootObject const& lootObject =
        AI_VALUE(LootObjectStack*, "available loot")->GetLoot(sPlayerbotAIConfig.lootDistance);

    if (!prevLoot.IsEmpty() && prevLoot.guid != lootObject.guid)
    {
        WorldPacket* packet = new WorldPacket(CMSG_LOOT_RELEASE, 8);
        *packet << prevLoot.guid;
        bot->GetSession()->QueuePacket(packet);
        // bot->GetSession()->HandleLootReleaseOpcode(packet);
    }

    // Provide a system to check if the game object id is disallowed in the user configurable list or not.
    // Check if the game object id is disallowed in the user configurable list or not.
    if (sPlayerbotAIConfig.disallowedGameObjects.find(lootObject.guid.GetEntry()) != sPlayerbotAIConfig.disallowedGameObjects.end())
    {
        return false;  // Game object ID is disallowed, so do not proceed
    }
    else
    {
        context->GetValue<LootObject>("loot target")->Set(lootObject);
        return true;
    }
}

bool LootAction::isUseful()
{
    return sPlayerbotAIConfig.freeMethodLoot || !bot->GetGroup() || bot->GetGroup()->GetLootMethod() != FREE_FOR_ALL;
}

enum ProfessionSpells
{
    ALCHEMY = 2259,
    BLACKSMITHING = 2018,
    COOKING = 2550,
    ENCHANTING = 7411,
    ENGINEERING = 49383,
    FIRST_AID = 3273,
    FISHING = 7620,
    HERB_GATHERING = 2366,
    INSCRIPTION = 45357,
    JEWELCRAFTING = 25229,
    MINING = 2575,
    SKINNING = 8613,
    TAILORING = 3908
};

bool OpenLootAction::Execute(Event /*event*/)
{
    LootObject lootObject = AI_VALUE(LootObject, "loot target");
    bool result = DoLoot(lootObject);
    if (result)
    {
        AI_VALUE(LootObjectStack*, "available loot")->Remove(lootObject.guid);
        context->GetValue<LootObject>("loot target")->Set(LootObject());
    }
    return result;
}

bool OpenLootAction::DoLoot(LootObject& lootObject)
{
    if (lootObject.IsEmpty())
        return false;

    Creature* creature = botAI->GetCreature(lootObject.guid);
    if (creature && bot->GetDistance(creature) > INTERACTION_DISTANCE - 2.0f)
        return false;

    // Dismount if the bot is mounted
    if (bot->IsMounted())
    {
        bot->Dismount();
        botAI->SetNextCheckDelay(sPlayerbotAIConfig.lootDelay); // Small delay to avoid animation issues
    }

    if (creature && creature->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE))
    {
        WorldPacket* packet = new WorldPacket(CMSG_LOOT, 8);
        *packet << lootObject.guid;
        bot->GetSession()->QueuePacket(packet);
        // bot->GetSession()->HandleLootOpcode(packet);
        botAI->SetNextCheckDelay(sPlayerbotAIConfig.lootDelay);
        return true;
    }

    if (bot->isMoving())
    {
        bot->StopMoving();
    }

    if (creature)
    {
        SkillType skill = creature->GetCreatureTemplate()->GetRequiredLootSkill();
        if (!CanOpenLock(skill, lootObject.reqSkillValue))
            return false;

        switch (skill)
        {
            case SKILL_ENGINEERING:
                return botAI->HasSkill(SKILL_ENGINEERING) ? botAI->CastSpell(ENGINEERING, creature) : false;
            case SKILL_HERBALISM:
                return botAI->HasSkill(SKILL_HERBALISM) ? botAI->CastSpell(32605, creature) : false;
            case SKILL_MINING:
                return botAI->HasSkill(SKILL_MINING) ? botAI->CastSpell(32606, creature) : false;
            default:
                return botAI->HasSkill(SKILL_SKINNING) ? botAI->CastSpell(SKINNING, creature) : false;
        }
    }

    GameObject* go = botAI->GetGameObject(lootObject.guid);
    if (go && bot->GetDistance(go) > INTERACTION_DISTANCE - 2.0f)
        return false;

    if (go && (go->GetGoState() != GO_STATE_READY))
        return false;

    // This prevents dungeon chests like Tribunal Chest (Halls of Stone) from being ninja'd by the bots
    if (go && go->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_INTERACT_COND))
        return false;

    // This prevents raid chests like Gunship Armory (ICC) from being ninja'd by the bots
    if (go && go->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_NOT_SELECTABLE))
        return false;

    if (lootObject.skillId == SKILL_MINING)
        return botAI->HasSkill(SKILL_MINING) ? botAI->CastSpell(MINING, bot) : false;

    if (lootObject.skillId == SKILL_HERBALISM)
        return botAI->HasSkill(SKILL_HERBALISM) ? botAI->CastSpell(HERB_GATHERING, bot) : false;

    uint32 spellId = GetOpeningSpell(lootObject);
    if (!spellId)
        return false;

    return botAI->CastSpell(spellId, bot);
}

uint32 OpenLootAction::GetOpeningSpell(LootObject& lootObject)
{
    if (GameObject* go = botAI->GetGameObject(lootObject.guid))
        if (go->isSpawned())
            return GetOpeningSpell(lootObject, go);

    return 0;
}

uint32 OpenLootAction::GetOpeningSpell(LootObject& lootObject, GameObject* go)
{
    for (PlayerSpellMap::iterator itr = bot->GetSpellMap().begin(); itr != bot->GetSpellMap().end(); ++itr)
    {
        uint32 spellId = itr->first;

        if (itr->second->State == PLAYERSPELL_REMOVED || !itr->second->Active)
            continue;

        if (spellId == MINING || spellId == HERB_GATHERING)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;

        if (CanOpenLock(lootObject, spellInfo, go))
            return spellId;
    }

    for (uint32 spellId = 0; spellId < sSpellMgr->GetSpellInfoStoreSize(); spellId++)
    {
        if (spellId == MINING || spellId == HERB_GATHERING)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;

        if (CanOpenLock(lootObject, spellInfo, go))
            return spellId;
    }

    return sPlayerbotAIConfig.openGoSpell;
}

bool OpenLootAction::CanOpenLock(LootObject& /*lootObject*/, SpellInfo const* spellInfo, GameObject* go)
{
    for (uint8 effIndex = 0; effIndex <= EFFECT_2; effIndex++)
    {
        if (spellInfo->Effects[effIndex].Effect != SPELL_EFFECT_OPEN_LOCK &&
            spellInfo->Effects[effIndex].Effect != SPELL_EFFECT_SKINNING)
            return false;

        uint32 lockId = go->GetGOInfo()->GetLockId();
        if (!lockId)
            return false;

        LockEntry const* lockInfo = sLockStore.LookupEntry(lockId);
        if (!lockInfo)
            return false;

        for (uint8 j = 0; j < 8; ++j)
        {
            switch (lockInfo->Type[j])
            {
                /*
                case LOCK_KEY_ITEM:
                    return true;
                */
                case LOCK_KEY_SKILL:
                {
                    if (uint32(spellInfo->Effects[effIndex].MiscValue) != lockInfo->Index[j])
                        continue;

                    uint32 skillId = SkillByLockType(LockType(lockInfo->Index[j]));
                    if (skillId == SKILL_NONE)
                        return true;

                    if (CanOpenLock(skillId, lockInfo->Skill[j]))
                        return true;
                }
            }
        }
    }

    return false;
}

bool OpenLootAction::CanOpenLock(uint32 skillId, uint32 reqSkillValue)
{
    uint32 skillValue = bot->GetSkillValue(skillId);
    return skillValue >= reqSkillValue || !reqSkillValue;
}

uint32 StoreLootAction::RoundPrice(double price)
{
    if (price < 100)
    {
        return (uint32)price;
    }

    if (price < 10000)
    {
        return (uint32)(price / 100.0) * 100;
    }

    if (price < 100000)
    {
        return (uint32)(price / 1000.0) * 1000;
    }

    return (uint32)(price / 10000.0) * 10000;
}

namespace
{
    // Per-house scan cache, refreshed by ONE full scan of the house's auction map every few
    // minutes. Auctions aren't indexed by owner or entry - a full scan is the only way to count a
    // bot's live listings (per-bot flood cap) or an item entry's live listings (per-item flood cap
    // and, via BotAuctionMarket::GetCachedAuctionCountForEntry(), the expired-overpopulation item
    // sink in BotAuctionOutcomeScript). The single scan fills BOTH maps - owner -> listing count
    // for EVERY owner in the house plus entry -> listing count - so the house is walked once per
    // TTL window instead of once per selling bot. Keyed by houseId (two-sided-interaction config
    // folds everything to Neutral upstream of here, so the key stays consistent). Mutex-guarded
    // because bot AI can run from map-update contexts.
    struct HouseScanCounts
    {
        time_t validUntil = 0;
        std::unordered_map<uint32, uint32> entryCounts;
        std::unordered_map<ObjectGuid, uint32> ownerCounts;
    };

    constexpr time_t BOT_AUCTION_COUNT_CACHE_TTL = 3 * MINUTE;

    std::mutex botAuctionCountLock;
    std::unordered_map<uint32, HouseScanCounts> houseScanCountCache;
}

uint32 BotAuctionMarket::GetMaxAuctionsPerItem()
{
    // Per-house cap on simultaneously active listings of a single item entry, 0 = off. Shared by
    // the listing-side skip in AuctionItem() and the expiry-side item sink in
    // BotAuctionOutcomeScript.
    static uint32 const maxAuctionsPerItem = sConfigMgr->GetOption<uint32>("AiPlayerbot.MaxAuctionsPerItem", 25);
    return maxAuctionsPerItem;
}

uint32 BotAuctionMarket::GetCachedAuctionCountForEntry(uint32 houseId, uint32 itemEntry)
{
    std::lock_guard<std::mutex> lock(botAuctionCountLock);
    auto const houseItr = houseScanCountCache.find(houseId);
    if (houseItr == houseScanCountCache.end())
        return 0;

    // A stale cache means no bot has listed in this house for a few minutes; report "unknown" (0)
    // rather than a snapshot old enough to have drifted - the expiry-side caller destroys items
    // based on this number, so under-reporting (item survives) is the safe direction.
    if (houseItr->second.validUntil <= GameTime::GetGameTime().count())
        return 0;

    auto const entryItr = houseItr->second.entryCounts.find(itemEntry);
    return entryItr != houseItr->second.entryCounts.end() ? entryItr->second : 0;
}

// This was previously dead code (wrapped in a block comment) written against an older
// AuctionHouseMgr/AuctionEntry API that has since drifted - see docs/session-improvements-2026-07-21.md
// item 6. Rewritten against the current core API (cross-checked against the real player packet
// path in AuctionHouseHandler.cpp::HandleAuctionSellItem):
//   - AuctionEntry's fields are now item_guid/item_template/expire_time (ObjectGuid owner/bidder),
//     not the old itemGuidLow/itemTemplate/expireTime (uint32 owner/bidder) layout.
//   - AuctionHouseMgr::GetAuctionHouseEntry(uint32) no longer exists; the faction->AuctionHouseEntry
//     lookup is AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(), and it already folds in
//     CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION (returns the Neutral DBC row when that's set), so no
//     extra branching is needed here.
//   - AuctionHouseObject* AuctionHouseMgr::GetAuctionsMap() takes the faction TEMPLATE id (uint32),
//     not an AuctionHouseEntry*; it independently folds in the same two-sided-config check.
//   - sAhBotConfig (mod-ah-bot's config) doesn't apply to this module at all - that's a separate,
//     disabled, vendored module (see doc item 6.4) - replaced with a small self-contained chance.
//   - Yes, this needs a live auctioneer NPC in range: both GetAuctionHouseEntryFromFactionTemplate()
//     and GetAuctionDeposit() key off the auctioneer's own faction template/DBC entry, exactly like
//     the real "talk to an Auctioneer" packet handler does - there's no "list remotely" API. Callers
//     (SellVendorItemsVisitor, AutoAuctionSellAction) are expected to already be near one; this
//     function is also safe to call speculatively since it just returns false if none is in range.
bool StoreLootAction::AuctionItem(Item* item, PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();

    if (!item)
        return false;

    ItemTemplate const* proto = item->GetTemplate();
    if (!proto)
        return false;

    if (proto->Bonding == BIND_WHEN_PICKED_UP || proto->Bonding == BIND_QUEST_ITEM)
        return false;

    // Mirrors the sanity checks the real HandleAuctionSellItem packet handler runs before it lets
    // a player list an item - a conjured/timed/non-empty-bag/untradeable item would just get
    // rejected there too.
    if (!item->CanBeTraded() || item->IsNotEmptyBag() || item->GetTemplate()->HasFlag(ITEM_FLAG_CONJURED) ||
        item->GetUInt32Value(ITEM_FIELD_DURATION))
        return false;

    GuidVector npcs = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest npcs")->Get();
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

    AuctionHouseEntry const* ahEntry =
        AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(auctioneer->GetFaction());
    if (!ahEntry)
        return false;

    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
    if (!auctionHouse)
        return false;

    // Flood caps - without them, hundreds of autonomous sellers grow their listing footprint
    // without bound (per bot) and pile identical gathered mats sky-high (per item). Both counts
    // come from the same cached full house scan (one iteration fills the per-owner counts for
    // every bot in the house plus the per-entry counts, so no bot ever needs its own rescan);
    // both maps are bumped locally on every successful listing so the caps hold within the TTL
    // window too.
    static uint32 const maxActiveAuctionsPerBot =
        sConfigMgr->GetOption<uint32>("AiPlayerbot.MaxActiveAuctionsPerBot", 10);
    uint32 const maxAuctionsPerItem = BotAuctionMarket::GetMaxAuctionsPerItem();
    if (maxActiveAuctionsPerBot || maxAuctionsPerItem)
    {
        std::lock_guard<std::mutex> lock(botAuctionCountLock);
        time_t const now = GameTime::GetGameTime().count();
        HouseScanCounts& cachedHouse = houseScanCountCache[ahEntry->houseId];
        if (cachedHouse.validUntil <= now)
        {
            cachedHouse.entryCounts.clear();
            cachedHouse.ownerCounts.clear();
            cachedHouse.validUntil = now + BOT_AUCTION_COUNT_CACHE_TTL;

            for (auto const& auctionPair : auctionHouse->GetAuctions())
            {
                ++cachedHouse.ownerCounts[auctionPair.second->owner];
                ++cachedHouse.entryCounts[auctionPair.second->item_template];
            }
        }

        if (maxActiveAuctionsPerBot)
        {
            auto const ownerItr = cachedHouse.ownerCounts.find(bot->GetGUID());
            if (ownerItr != cachedHouse.ownerCounts.end() && ownerItr->second >= maxActiveAuctionsPerBot)
                return false;
        }

        if (maxAuctionsPerItem)
        {
            auto const entryItr = cachedHouse.entryCounts.find(item->GetEntry());
            if (entryItr != cachedHouse.entryCounts.end() && entryItr->second >= maxAuctionsPerItem)
            {
                LOG_DEBUG("playerbots", "Playerbot {} skipping auction of {}: {} active listings >= per-item cap {}",
                    bot->GetName(), proto->Name1, entryItr->second, maxAuctionsPerItem);
                return false;
            }
        }
    }

    uint32 stackCount = item->GetCount();

    double basePrice = double(stackCount) * double(proto->BuyPrice) * sRandomPlayerbotMgr.GetBuyMultiplier(bot);
    if (basePrice <= 0.0)
        return false;

    // Occasional below-vendor-price listing so bot-posted auctions don't all read identically.
    // (The old code read this ratio from mod-ah-bot's config, which doesn't apply to this module -
    // see the comment above - so it's just a fixed 1-in-4 chance here instead.)
    if (!urand(0, 3))
        basePrice = basePrice * 100.0 / urand(100, 200);

    uint32 bidPrice = RoundPrice(basePrice);
    if (!bidPrice)
        return false;

    uint32 buyoutPrice = RoundPrice(double(urand(bidPrice, 4 * bidPrice / 3)));

    // Price floor: the occasional underprice roll above (and RoundPrice truncation) must never
    // produce a listing below what a vendor would pay for the stack - otherwise buying the bot out
    // and vendoring the item is free money, and the bot would have been better off vendoring it
    // itself. Clamp bid first, then buyout, so buyout >= bid is preserved.
    uint32 const vendorValue = proto->SellPrice * stackCount + 1;
    bidPrice = std::max(bidPrice, vendorValue);
    buyoutPrice = std::max(buyoutPrice, bidPrice);

    // 48h - the longest listing a real client can request (HandleAuctionSellItem only accepts
    // 1x/2x/4x MIN_AUCTION_TIME). Longer listings mean more time to find a buyer per deposit paid.
    // Deposit parity with the real handler: it computes the deposit from the UN-rated etime and
    // applies RATE_AUCTION_TIME only to the actual expiry, so do the same here.
    uint32 const etime = 4 * MIN_AUCTION_TIME;
    uint32 auctionTime = uint32(etime * sWorld->getRate(RATE_AUCTION_TIME));

    uint32 deposit = AuctionHouseMgr::GetAuctionDeposit(ahEntry, etime, item, stackCount);
    if (!bot->HasEnoughMoney(deposit))
        return false;

    bot->ModifyMoney(-int32(deposit));

    AuctionEntry* auctionEntry = new AuctionEntry();
    auctionEntry->Id = sObjectMgr->GenerateAuctionID();
    auctionEntry->houseId = AuctionHouseId(ahEntry->houseId);
    auctionEntry->item_guid = item->GetGUID();
    auctionEntry->item_template = item->GetEntry();
    auctionEntry->itemCount = item->GetCount();
    auctionEntry->owner = bot->GetGUID();
    auctionEntry->startbid = bidPrice;
    auctionEntry->bidder = ObjectGuid::Empty;
    auctionEntry->bid = 0;
    auctionEntry->buyout = buyoutPrice;
    auctionEntry->expire_time = GameTime::GetGameTime().count() + auctionTime;
    auctionEntry->deposit = deposit;
    auctionEntry->auctionHouseEntry = ahEntry;

    sAuctionMgr->AddAItem(item);
    auctionHouse->AddAuction(auctionEntry);

    bot->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    item->DeleteFromInventoryDB(trans);
    item->SaveToDB(trans);
    auctionEntry->SaveToDB(trans);
    bot->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);

    // Keep the cached per-owner listing count and per-house entry count honest between full
    // rescans (no-op if the caps are disabled - the house cache entry only exists when the cap
    // checks above ran).
    {
        std::lock_guard<std::mutex> lock(botAuctionCountLock);
        auto const houseItr = houseScanCountCache.find(ahEntry->houseId);
        if (houseItr != houseScanCountCache.end())
        {
            ++houseItr->second.ownerCounts[bot->GetGUID()];
            ++houseItr->second.entryCounts[auctionEntry->item_template];
        }
    }

    LOG_INFO("playerbots", "Playerbot {} listed {} of {} on the auction house for {}..{} copper (deposit {})",
              bot->GetName(), stackCount, proto->Name1, bidPrice, buyoutPrice, deposit);

    return true;
}

bool StoreLootAction::Execute(Event event)
{
    WorldPacket p(event.getPacket());  // (8+1+4+1+1+4+4+4+4+4+1)
    ObjectGuid guid;
    uint8 loot_type;
    uint32 gold = 0;
    uint8 items = 0;

    p.rpos(0);
    p >> guid;       // 8 corpse guid
    p >> loot_type;  // 1 loot type

    if (p.size() > 10)
    {
        p >> gold;   // 4 money on corpse
        p >> items;  // 1 number of items on corpse
    }

    bot->SetLootGUID(guid);

    if (gold > 0)
    {
        WorldPacket* packet = new WorldPacket(CMSG_LOOT_MONEY, 0);
        bot->GetSession()->QueuePacket(packet);
        // bot->GetSession()->HandleLootMoneyOpcode(packet);
    }

    for (uint8 i = 0; i < items; ++i)
    {
        uint32 itemid;
        uint32 itemcount;
        uint8 lootslot_type;
        uint8 itemindex;

        p >> itemindex;
        p >> itemid;
        p >> itemcount;
        p.read_skip<uint32>();  // display id
        p.read_skip<uint32>();  // randomSuffix
        p.read_skip<uint32>();  // randomPropertyId
        p >> lootslot_type;     // 0 = can get, 1 = look only, 2 = master get

        if (lootslot_type != LOOT_SLOT_TYPE_ALLOW_LOOT && lootslot_type != LOOT_SLOT_TYPE_OWNER)
            continue;

        if (loot_type != LOOT_SKINNING && !IsLootAllowed(itemid, botAI))
            continue;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemid);
        if (!proto)
            continue;

        if (!botAI->HasActivePlayerMaster() && AI_VALUE(uint8, "bag space") > 80)
        {
            uint32 maxStack = proto->GetMaxStackSize();
            if (maxStack == 1)
                continue;

            std::vector<Item*> found = parseItems(chat->FormatItem(proto));

            bool hasFreeStack = false;

            for (auto stack : found)
            {
                if (stack->GetCount() + itemcount < maxStack)
                {
                    hasFreeStack = true;
                    break;
                }
            }

            if (!hasFreeStack)
                continue;
        }

        Player* master = botAI->GetMaster();
        if (sRandomPlayerbotMgr.IsRandomBot(bot) && master)
        {
            uint32 price = itemcount * proto->BuyPrice * sRandomPlayerbotMgr.GetBuyMultiplier(bot) + gold;
            if (price)
                sRandomPlayerbotMgr.AddTradeDiscount(bot, master, price);

            if (Group* group = bot->GetGroup())
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                    if (ref->GetSource() != bot)
                        GuildTaskMgr::instance().CheckItemTask(itemid, itemcount, ref->GetSource(), bot);
        }

        WorldPacket* packet = new WorldPacket(CMSG_AUTOSTORE_LOOT_ITEM, 1);
        *packet << itemindex;
        bot->GetSession()->QueuePacket(packet);
        // bot->GetSession()->HandleAutostoreLootItemOpcode(packet);
        botAI->SetNextCheckDelay(sPlayerbotAIConfig.lootDelay);

        if (proto->Quality > ITEM_QUALITY_NORMAL && !urand(0, 50) && botAI->HasStrategy("emote", BOT_STATE_NON_COMBAT) && sPlayerbotAIConfig.randomBotEmote)
            botAI->PlayEmote(TEXT_EMOTE_CHEER);

        if (proto->Quality >= ITEM_QUALITY_RARE && !urand(0, 1) && botAI->HasStrategy("emote", BOT_STATE_NON_COMBAT) && sPlayerbotAIConfig.randomBotEmote)
            botAI->PlayEmote(TEXT_EMOTE_CHEER);

        BroadcastHelper::BroadcastLootingItem(botAI, bot, proto);
    }

    AI_VALUE(LootObjectStack*, "available loot")->Remove(guid);

    // release loot
    WorldPacket* packet = new WorldPacket(CMSG_LOOT_RELEASE, 8);
    *packet << guid;
    bot->GetSession()->QueuePacket(packet);
    // bot->GetSession()->HandleLootReleaseOpcode(packet);
    return true;
}

bool StoreLootAction::IsLootAllowed(uint32 itemid, PlayerbotAI* botAI)
{
    AiObjectContext* context = botAI->GetAiObjectContext();
    LootStrategy* lootStrategy = AI_VALUE(LootStrategy*, "loot strategy");

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemid);
    if (!proto)
        return false;

    std::set<uint32>& lootItems = AI_VALUE(std::set<uint32>&, "always loot list");
    if (lootItems.find(itemid) != lootItems.end())
        return true;

    uint32 max = proto->MaxCount;
    if (max > 0 && botAI->GetBot()->HasItemCount(itemid, max, true))
        return false;

    if (proto->StartQuest)
    {
        return true;
    }

    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 entry = botAI->GetBot()->GetQuestSlotQuestId(slot);
        Quest const* quest = sObjectMgr->GetQuestTemplate(entry);
        if (!quest)
            continue;

        for (uint8 i = 0; i < 4; i++)
        {
            if (quest->RequiredItemId[i] == itemid)
            {
                // if (AI_VALUE2(uint32, "item count", proto->Name1) < quest->RequiredItemCount[i])
                // {
                //     if (botAI->GetMaster() && sPlayerbotAIConfig.syncQuestWithPlayer)
                //         return false; //Quest is autocomplete for the bot so no item needed.
                // }

                return true;
            }
        }
    }

    // if (proto->Bonding == BIND_QUEST_ITEM ||  //Still testing if it works ok without these lines.
    //     proto->Bonding == BIND_QUEST_ITEM1 || //Eventually this has to be removed.
    //     proto->Class == ITEM_CLASS_QUEST)
    //{

    bool canLoot = lootStrategy->CanLoot(proto, context);
    // if (canLoot && proto->Bonding == BIND_WHEN_PICKED_UP && botAI->HasActivePlayerMaster())
    // canLoot = sPlayerbotAIConfig.IsInRandomAccountList(botAI->GetBot()->GetSession()->GetAccountId());

    return canLoot;
}

bool ReleaseLootAction::Execute(Event /*event*/)
{
    GuidVector gos = context->GetValue<GuidVector>("nearest game objects")->Get();
    for (ObjectGuid const guid : gos)
    {
        WorldPacket* packet = new WorldPacket(CMSG_LOOT_RELEASE, 8);
        *packet << guid;
        bot->GetSession()->QueuePacket(packet);
    }

    GuidVector corpses = context->GetValue<GuidVector>("nearest corpses")->Get();
    for (ObjectGuid const guid : corpses)
    {
        WorldPacket* packet = new WorldPacket(CMSG_LOOT_RELEASE, 8);
        *packet << guid;
        bot->GetSession()->QueuePacket(packet);
    }

    return true;
}
