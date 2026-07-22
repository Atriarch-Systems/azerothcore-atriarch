#include "mod-ollama-chat_leaderoffer.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Group.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotOperations.h"
#include "PlayerbotWorldThreadProcessor.h"

namespace
{
    std::mutex g_LeaderOfferMutex;
    std::unordered_map<ObjectGuid, PendingLeaderOffer> g_LeaderOffers;

    // Leadership-request trigger phrases. Simple substring match against the lowercased
    // message so this list stays easy to extend without touching the matching logic.
    std::vector<std::string> const g_LeaderRequestPhrases =
    {
        "give me lead",
        "give me the lead",
        "let me lead",
        "can i lead",
        "make me leader"
    };

    // Deterministic yes/no confirmation keywords for a pending offer's reply. No LLM call
    // here: this gates a real Group::ChangeLeader, so the match stays plain and boring.
    std::vector<std::string> const g_AffirmativeReplies =
    {
        "y",
        "yes",
        "yeah",
        "yep",
        "sure",
        "please",
        "do it",
        "ok",
        "okay"
    };

    std::string ToLowerCopy(std::string const& text)
    {
        std::string result = text;
        std::transform(result.begin(), result.end(), result.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
    }

    // Trim leading/trailing whitespace. (The module-wide rtrim() in
    // mod-ollama-chat_handler.cpp also eats trailing punctuation, which would turn "yes!"
    // into "yes" -- fine for that caller, but here we want our own small, explicit trim so
    // this file doesn't depend on that helper's exact behavior.)
    std::string TrimCopy(std::string const& text)
    {
        size_t start = text.find_first_not_of(" \t\n\r");
        if (start == std::string::npos)
            return "";
        size_t end = text.find_last_not_of(" \t\n\r");
        return text.substr(start, end - start + 1);
    }

    bool IsAffirmativeReply(std::string const& lowerTrimmedMsg)
    {
        for (std::string const& word : g_AffirmativeReplies)
        {
            if (lowerTrimmedMsg == word)
                return true;
        }
        return false;
    }

    bool ContainsLeaderRequestPhrase(std::string const& lowerMsg)
    {
        for (std::string const& phrase : g_LeaderRequestPhrases)
        {
            if (lowerMsg.find(phrase) != std::string::npos)
                return true;
        }
        return false;
    }

    // Exact mechanism PassLeadershipToMasterAction::Execute uses to hand real leadership to
    // a real player (PlayerbotOperations.h / PlayerbotWorldThreadProcessor.h).
    void QueueLeaderTransfer(Player* bot, Player* newLeader)
    {
        auto setLeaderOp = std::make_unique<GroupSetLeaderOperation>(bot->GetGUID(), newLeader->GetGUID());
        PlayerbotWorldThreadProcessor::instance().QueueOperation(std::move(setLeaderOp));
    }

    // Plain canned line, no LLM. Party vs raid mirrors the routing already used for normal
    // bot chat replies in mod-ollama-chat_handler.cpp.
    void SayLeaderReply(PlayerbotAI* botAI, Player* bot, std::string const& text)
    {
        if (bot->GetGroup() && bot->GetGroup()->isRaidGroup())
            botAI->SayToRaid(text);
        else
            botAI->SayToParty(text);
    }
}

bool TryHandleLeaderOfferChat(Player* player, PlayerbotAI* botAI, std::string const& msg)
{
    if (!player || !botAI)
        return false;

    Player* bot = botAI->GetBot();
    if (!bot)
        return false;

    ObjectGuid const playerGuid = player->GetGUID();
    ObjectGuid const botGuid = bot->GetGUID();

    // Step 1: is this message a reply to a live offer waiting on this exact player, from
    // this exact bot?
    {
        std::unique_lock<std::mutex> lock(g_LeaderOfferMutex);
        auto it = g_LeaderOffers.find(playerGuid);
        if (it != g_LeaderOffers.end())
        {
            if (time(nullptr) > it->second.expiresAt)
            {
                // Expired: clear it and fall through to step 2 as if there were no offer.
                g_LeaderOffers.erase(it);
            }
            else if (it->second.botGuid == botGuid)
            {
                // Live offer addressed to this specific bot -- resolve it one way or another.
                g_LeaderOffers.erase(it);
                lock.unlock();

                std::string const lowerTrimmedMsg = ToLowerCopy(TrimCopy(msg));
                if (IsAffirmativeReply(lowerTrimmedMsg))
                {
                    QueueLeaderTransfer(bot, player);
                    SayLeaderReply(botAI, bot, "All yours.");
                }
                // Anything else cancels the offer with zero state change -- never retry,
                // never guess.
                return true;
            }
            // else: a live offer exists for this player but it's addressed to a different
            // bot -- leave it alone and fall through, in case this message is *also* a fresh
            // leadership request directed at this (different) bot.
        }
    }

    // Step 2: does this look like a leadership request, and is this bot actually the group's
    // current real leader? Only the bot holding real leadership should ever offer to hand it
    // off.
    if (!bot->GetGroup() || !bot->GetGroup()->IsLeader(bot->GetGUID()))
        return false;

    if (!ContainsLeaderRequestPhrase(ToLowerCopy(msg)))
        return false;

    {
        std::lock_guard<std::mutex> lock(g_LeaderOfferMutex);
        g_LeaderOffers[playerGuid] = PendingLeaderOffer
        {
            botGuid,
            time(nullptr) + static_cast<time_t>(sPlayerbotAIConfig.lfgLeaderOfferTimeoutSeconds)
        };
    }

    SayLeaderReply(botAI, bot, "Do you want lead?");
    return true;
}
