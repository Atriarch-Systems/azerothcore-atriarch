#ifndef MOD_OLLAMA_CHAT_RUMORS_H
#define MOD_OLLAMA_CHAT_RUMORS_H

#include "Player.h"
#include <string>

// Rumor system (seeker experiment support, docs/seeker-selenwe.md):
// bots remember real players they recently saw and can mention them in chat.
// Bounded per bot (OllamaChat.RumorMaxSightings, default 5) with a long TTL
// (OllamaChat.RumorTTLDays, default 14) and persisted to the characters DB
// (mod_ollama_chat_rumors) so sightings survive server restarts.

void RumorRecordSighting(Player* bot, Player* realPlayer);
std::string GetRumorPromptText(Player* bot);

// Persistence lifecycle (mirrors conversation-history handling)
void LoadRumorsFromDB();      // call once at startup after config load
void MaybeSaveRumorsToDB();   // call from a periodic update; throttled internally
void SaveRumorsToDB();        // immediate save + prune of expired rows

// Seeker journey integration (docs/seeker-selenwe.md, journey memory):
// chronicle chapters live in mod_ollama_seeker_chronicle (owned by
// mod-ollama-bot-buddy); the seeker's name is read from the shared config key
// OllamaBotControl.SeekerName.
std::string GetSeekerJourneyPromptText(Player* bot);
void RecordSeekerConversation(Player* possibleSeekerSender, Player* speakingBot, std::string const& text);

#endif // MOD_OLLAMA_CHAT_RUMORS_H
