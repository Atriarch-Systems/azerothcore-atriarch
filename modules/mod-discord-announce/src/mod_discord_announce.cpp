#include <queue>
#include <mutex>
#include <string>
#include <string>
#include <thread>
#include <vector>

#include "Chat.h"
#include "Config.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringFormat.h"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

#define sConfig Config::instance()
#define sHTTPManager HTTPManager::instance()

class HTTPManager;

struct Config
{
    std::string webhookID;
    std::string webhookToken;
    int8 playerDeath;
    int8 playerLevelUp; 
    bool enabled;
    bool announceGuildCreation;
    bool announcePlayerMaxLevel;
    bool announceWorldBossDeath;

    static Config* instance()
    {
        static Config instance;
        return &instance;
    }

    void LoadConfig()
    {
        enabled = sConfigMgr->GetOption<bool>("DiscordAnnounce.Enabled", false);
        webhookID = sConfigMgr->GetOption<std::string>("DiscordAnnounce.WebhookID", "");
        webhookToken = sConfigMgr->GetOption<std::string>("DiscordAnnounce.WebhookToken", "");

        if (webhookID == "" || webhookToken == "") 
        {
            enabled = false;
            LOG_ERROR(
                "server", 
                "MOD DiscordAnnounce: please provide a token and ID to enable the mod."
            );
        }

        announceGuildCreation = sConfigMgr->GetOption<bool>("DiscordAnnounce.GuildCreation", false);

        playerDeath = sConfigMgr->GetOption<int8>("DiscordAnnounce.PlayerDeath", -1);
        playerLevelUp = sConfigMgr->GetOption<int8>("DiscordAnnounce.PlayerLevelUp", -1);
        announcePlayerMaxLevel = sConfigMgr->GetOption<bool>("DiscordAnnounce.PlayerMaxLevel", false);
        announceWorldBossDeath = sConfigMgr->GetOption<bool>("DiscordAnnounce.WorldBossDeath", false);
    }
};

class HTTPManager 
{
    std::atomic_bool shouldStop;
    std::thread workerThread;
    std::queue<std::string> queue;
    std::mutex mutex;

    std::string webhookID;
    std::string webhookURL;

public: 
    static HTTPManager* instance() 
    {
        static HTTPManager instance;
        return &instance;
    }

    void Init()
    {
        webhookURL = "/api/webhooks/" + sConfig->webhookID + "/" + sConfig->webhookToken;
        webhookID = sConfig->webhookID;
        LOG_INFO("server", "MOD DiscordAnnounce: Using discord URL {}", webhookURL);
        queue = std::queue<std::string>();
        StartWorker();
    }

    void UpdateWebhookValues()
    {
        StopWorker();
        webhookURL = "/api/webhooks/" + sConfig->webhookID + "/" + sConfig->webhookToken;
        webhookID = sConfig->webhookID;
        StartWorker();
    }

    void EnqueueMessage(std::string& message)
    {
        std::scoped_lock lock(this->mutex);
        queue.push(message);
    }

    void StartWorker()
    {
        shouldStop.store(false);
        workerThread = std::thread(&HTTPManager::HTTPWorkerThread, this); 
    }

    void StopWorker() 
    {
        shouldStop.store(true);
        workerThread.join();
    }

private:
    void HTTPWorkerThread()
    {
        httplib::Client client("https://discord.com");
        while(true)
        {
            std::scoped_lock lock(this->mutex);

            // Ensure all messages were sent before stopping
            if (queue.empty())
            {
                if (shouldStop.load())
                {
                    break;
                }

                continue;
            }

            std::string message = queue.front();
            queue.pop();
            std::string formattedMessage = Acore::StringFormat(
                R"({{"content": "{}", "id": "{}"}})", 
                message, 
                webhookID
            );
            auto response = client.Post(webhookURL, formattedMessage, "application/json");
            if (!response) 
            {
                LOG_ERROR(
                    "server", 
                    "MOD Announce: Error while sending message: {}", 
                    httplib::to_string(response.error())
                );
            }
        }
    }
};

class AnnounceWorld : public WorldScript
{
public:
    AnnounceWorld() : WorldScript("AnnounceWorld") { }

    void OnAfterConfigLoad(bool reload) override
    {
        if (reload)
        {
            sConfig->LoadConfig();         
            sHTTPManager->UpdateWebhookValues();
        }
    }
};

class AnnounceGuild : public GuildScript
{
public:
    AnnounceGuild() : GuildScript("AnnounceGuild") { }

    void OnCreate(Guild* guild, Player* leader, const std::string& name) override
    {
        if (sConfig->enabled && sConfig->announceGuildCreation)
        {
            std::string message = Acore::StringFormat("{} just created the guild: {}", leader->GetName(), name);
            LOG_INFO("server", "MOD Announce: {}", message);
            sHTTPManager->EnqueueMessage(message);
            return;
        }
    }
};

class AnnouncePlayer : public PlayerScript
{
public:
    AnnouncePlayer() : PlayerScript("AnnouncePlayer") { }

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        if (!sConfig->enabled) return; 

        uint8 level = player->GetLevel(); 
        if (sConfig->announcePlayerMaxLevel && player->IsMaxLevel())
        {
            std::string message = Acore::StringFormat("{} reached max level.", player->GetName());
            LOG_INFO("server", "MOD Announce: {}", message);
            sHTTPManager->EnqueueMessage(message);
            // Prevent potential double announces if level announce is available.
            return;
        }

        if (
            sConfig->playerLevelUp != -1 && 
            (level >= (uint8) sConfig->playerLevelUp)
        ) {
            std::string message = Acore::StringFormat("{} reached level {}.", player->GetName(), level);
            LOG_INFO("server",  "MOD DiscordAnnounce: {}", message);
            sHTTPManager->EnqueueMessage(message);
        }
    }

    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
    {
        if (!sConfig->enabled || sConfig->playerDeath == -1) return; 

        uint8 level = killed->GetLevel(); 
        if (level >= (uint8) sConfig->playerDeath)
        {
            std::string message = Acore::StringFormat("{} died at level {}, killed by: {}.", killed->GetName(), level, killer->GetName());
            LOG_INFO("server", "MOD DiscordAnnounce: {}", message);
            sHTTPManager->EnqueueMessage(message);
        }
    }

    // Inspired by https://github.com/azerothcore/mod-boss-announcer
    void OnPlayerCreatureKill(Player* player, Creature* boss) override
    {
        if (sConfig->announceWorldBossDeath && boss->GetMap()->IsRaid() && boss->GetLevel() > 80 && boss->IsDungeonBoss())
        {
            std::string message = Acore::StringFormat("World Boss {} has been killed.", boss->GetName());
            LOG_INFO("server", "MOD DiscordAnnounce: {}", message);
            sHTTPManager->EnqueueMessage(message);
        }
    }
};

using namespace Acore::ChatCommands;

class AnnounceCommand : public CommandScript 
{
public:
    AnnounceCommand() : CommandScript("AnnounceCommand") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable subCommands =
        {
            { "info", HandleDiscordAnnounceInfo, SEC_MODERATOR, Console::Yes },
        };

        static ChatCommandTable commandTable =
        { 
            { "discordannounce", subCommands },
        };

        return commandTable;
    }

    static bool HandleDiscordAnnounceInfo(ChatHandler* handler)
    {
        handler->PSendSysMessage(R"(DiscordAnnounce info:
Enabled? {}
WebhookID: {}
WehookToken: {}
PlayerDeath: {}
PlayerLevelUp: {}
Announce guild creation? {}
Announce player max level? {}
Announce World Boss death? {})",
            sConfig->enabled,
            sConfig->webhookID,
            sConfig->webhookToken,
            sConfig->playerDeath,
            sConfig->playerLevelUp,
            sConfig->announceGuildCreation,
            sConfig->announcePlayerMaxLevel,
            sConfig->announceWorldBossDeath
        );

        return true;
    }
};

void Addmod_discord_announceScripts()
{
    sConfig->LoadConfig();
    sHTTPManager->Init();
    new AnnouncePlayer();
    new AnnounceGuild();
    new AnnounceWorld();
    new AnnounceCommand();
}

/*
    This file is part of mod-discord-announce.

    mod-discord-announce is free software: you can redistribute it and/or modify it under the terms 
    of the GNU Affero General Public License as published by the Free Software Foundation, either 
    version 3 of the License, or any later version.
    mod-weekend-xp is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; 
    without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See 
    the GNU Affero General Public License for more details.

    You should have received a copy of the GNU General Public License along with Foobar. If not, 
    see <https://www.gnu.org/licenses/>. 
*/
