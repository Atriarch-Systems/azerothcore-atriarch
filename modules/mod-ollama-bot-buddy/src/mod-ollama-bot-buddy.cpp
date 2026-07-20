#include "mod-ollama-bot-buddy_config.h"
#include "mod-ollama-bot-buddy_loop.h"
#include "mod-ollama-bot-buddy_handler.h"
#include "mod-ollama-bot-buddy_rebirth.h"
#include "mod-ollama-bot-buddy_raidlab.h"
#include "mod-ollama-bot-buddy_intent.h"

#include "Log.h"

void Addmod_ollama_bot_buddyScripts()
{
    new OllamaBotControlConfigWorldScript();
    LOG_INFO("server.loading", "Registering mod-ollama-bot-buddy scripts.");
    new OllamaBotControlLoop();
    new BotBuddyChatHandler();
    new SeekerEventScript();
    new BotRebirthLoop();
    new RaidLabCommandScript();
    new RaidLabWatcher();
    new IntentWorldScript();
}
