#ifndef MOD_OLLAMA_CHAT_LEADEROFFER_H
#define MOD_OLLAMA_CHAT_LEADEROFFER_H

#include <ctime>
#include <string>
#include "ObjectGuid.h"

class Player;
class PlayerbotAI;

// Chat-based group-leadership handoff (docs/dungeon-leadership-and-summon.md, section 2).
// A real player can ask a bot that currently holds real Group leadership for it via chat.
// Confirmation is a deterministic yes/no keyword match -- never routed through the LLM,
// since it gates a real Group::ChangeLeader call -- before the transfer is queued through
// the same PlayerbotWorldThreadProcessor -> GroupSetLeaderOperation path that
// PassLeadershipToMasterAction already uses today.

// A pending "do you want lead?" offer waiting on a specific requesting player's reply.
struct PendingLeaderOffer
{
    ObjectGuid botGuid;
    time_t expiresAt;
};

// Entry point: call for each bot candidate a chat message is relevant to, before any
// LLM-dispatch work happens for that bot (see PlayerBotChatHandler::ProcessChat in
// mod-ollama-chat_handler.cpp). Returns true if this feature fully handled the message for
// this specific bot -- caller should skip normal chat/LLM processing for that bot -- or
// false if the message doesn't concern this feature for this bot and normal processing
// should continue as usual.
bool TryHandleLeaderOfferChat(Player* player, PlayerbotAI* botAI, std::string const& msg);

#endif // MOD_OLLAMA_CHAT_LEADEROFFER_H
