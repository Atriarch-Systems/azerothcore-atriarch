#include "ScriptMgr.h"
#include <string>
#include <Group.h>
#include <Channel.h>

extern std::unordered_map<uint64_t, std::deque<std::pair<std::string, std::string>>> botPlayerMessages;
extern std::mutex botPlayerMessagesMutex;

// Observes player chat via the CanUseChat gates (the OnPlayerChat observer
// hooks were removed from the core). Always returns true — this script never
// blocks chat, it only records messages addressed to buddy bots.
class BotBuddyChatHandler : public PlayerScript
{
public:
    BotBuddyChatHandler() : PlayerScript("BotBuddyChatHandler", {
        PLAYERHOOK_CAN_PLAYER_USE_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT,
    }) {}

    bool OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg) override;
    bool OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Group* group) override;
    bool OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Channel* channel) override;

private:
    void ProcessChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Channel* channel = nullptr);
};
