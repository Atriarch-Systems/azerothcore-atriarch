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
    // Matched against the LEADING word of the reply (see MatchesLeadingWord), so "yes
    // please", "sure thing", "yeah man" all confirm.
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

    // Explicit declines. Matched the same leading-word way, so "no thanks" / "nah, keep it"
    // decline cleanly. Any reply that is neither affirmative nor negative leaves the offer
    // pending until it expires on its own.
    std::vector<std::string> const g_NegativeReplies =
    {
        "no",
        "nah",
        "nope",
        "cancel",
        "nevermind",
        "never mind"
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

    // True when the reply's LEADING word (everything before the first space/punctuation of
    // the already-lowercased, trimmed message) equals one of the given keywords. Multi-word
    // keywords ("do it", "never mind") can never equal a single token, so those are matched
    // as a leading phrase followed by end-of-message or a word boundary instead.
    bool MatchesLeadingWord(std::string const& lowerTrimmedMsg, std::vector<std::string> const& words)
    {
        if (lowerTrimmedMsg.empty())
            return false;

        // Apostrophe deliberately NOT a token boundary: "y'know..." must tokenize as
        // "y'know" (matching nothing), not "y" (an affirmative) -- otherwise a reply
        // like "y'know what, never mind" would CONFIRM the transfer it was declining.
        size_t const tokenEnd = lowerTrimmedMsg.find_first_of(" \t.,!?;:\"");
        std::string const token = (tokenEnd == std::string::npos)
            ? lowerTrimmedMsg
            : lowerTrimmedMsg.substr(0, tokenEnd);

        for (std::string const& word : words)
        {
            if (word == token)
                return true;

            if (word.find(' ') == std::string::npos)
                continue;

            if (lowerTrimmedMsg.compare(0, word.size(), word) != 0)
                continue;

            if (lowerTrimmedMsg.size() == word.size())
                return true;

            char const next = lowerTrimmedMsg[word.size()];
            if (next == ' ' || std::ispunct(static_cast<unsigned char>(next)))
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
                // Live offer addressed to this specific bot. Only an explicit yes or an
                // explicit no resolves it; anything else ("sec", "one moment", ordinary
                // chatter) leaves it pending so it can still be answered until it expires
                // on its own.
                std::string const lowerTrimmedMsg = ToLowerCopy(TrimCopy(msg));
                if (MatchesLeadingWord(lowerTrimmedMsg, g_AffirmativeReplies))
                {
                    g_LeaderOffers.erase(it);
                    lock.unlock();

                    // Leadership can have moved (disband, manual /promote, another module)
                    // between the offer and this confirmation -- queueing the transfer
                    // anyway would be acting on authority the bot no longer has.
                    if (!bot->GetGroup() || !bot->GetGroup()->IsLeader(bot->GetGUID()))
                    {
                        SayLeaderReply(botAI, bot, "I'm not the leader anymore.");
                        return true;
                    }

                    QueueLeaderTransfer(bot, player);
                    SayLeaderReply(botAI, bot, "All yours.");
                    return true;
                }
                if (MatchesLeadingWord(lowerTrimmedMsg, g_NegativeReplies))
                {
                    g_LeaderOffers.erase(it);
                    lock.unlock();

                    SayLeaderReply(botAI, bot, "Alright, I'll keep lead.");
                    return true;
                }
                // Neither: fall through with the offer intact. Step 2 below may still
                // treat this same message as a fresh leadership request (re-asking simply
                // refreshes the offer), and returning false otherwise lets normal chat
                // processing continue untouched.
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
