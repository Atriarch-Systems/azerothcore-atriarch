#include "mod_accountbound.h"

void AccountBound::OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement)
{
    if (player->IsGameMaster() && !EnableGamemasters)
    {
        return;
    }

    if (EnableMounts)
    {
        if (achievement->ID == ACHIEVEMENT_APPRENTICE || achievement->ID == ACHIEVEMENT_JOURNEYMAN || achievement->ID == ACHIEVEMENT_EXPERT || achievement->ID == ACHIEVEMENT_ARTISAN)
        {
            LearnMounts(player);
        }
    }
}

void AccountBound::OnPlayerAfterStoreOrEquipNewItem(Player* player, uint32 /*vendorslot*/, Item* item, uint8 /*count*/, uint8 /*bag*/, uint8 /*slot*/, ItemTemplate const* /*pProto*/, Creature* /*pVendor*/, VendorItem const* /*crItem*/, bool /*bStore*/)
{
    if (player->IsGameMaster() && !EnableGamemasters)
    {
        return;
    }

    if (!item || !item->GetTemplate())
    {
        return;
    }

    if (item->GetTemplate()->Quality != ITEM_QUALITY_HEIRLOOM || item->GetEntry() == 49177)
    {
        return;
    }

    if (EnableHeirlooms)
    {
        SaveHeirloom(player, item);
    }
}

void AccountBound::OnPlayerLevelChanged(Player* player, uint8 /*oldlevel*/)
{
    if (player->IsGameMaster() && !EnableGamemasters)
    {
        return;
    }

    if (EnableMounts)
    {
        LearnMounts(player);
    }
}

void AccountBound::OnPlayerLogin(Player* player)
{
    if (player->IsGameMaster() && !EnableGamemasters)
    {
        return;
    }

    if (EnableCompanions)
    {
        LearnCompanions(player);
    }

    if (EnableMounts)
    {
        LearnMounts(player);
    }
}

void AccountBound::OnPlayerSave(Player* player)
{
    if (player->IsGameMaster() && !EnableGamemasters)
    {
        return;
    }

    if (EnableCompanions)
    {
        SaveCompanions(player);
    }

    if (EnableMounts)
    {
        SaveMounts(player);
    }
}
