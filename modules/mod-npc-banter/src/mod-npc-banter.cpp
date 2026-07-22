#include "mod_npc_banter_config.h"
#include "mod_npc_banter_registry.h"
#include "mod_npc_banter_worldscript.h"
#include "Log.h"

// Loader function name is mechanically derived by ConfigureModules.cmake from
// this module's directory name ("mod-npc-banter" -> hyphens become
// underscores -> "mod_npc_banter", then "Add" + that + "Scripts()"), the same
// way every other module's loader is named and invoked (e.g. mod-ollama-chat
// -> Addmod_ollama_chatScripts(), mod-ollama-bot-buddy -> Addmod_ollama_bot_buddyScripts()).
// No manual wiring outside this file is required - CMakeLists.txt/ModulesLoader.cpp.in.cmake
// forward-declare and invoke this exact symbol automatically once the module
// directory is discovered.
void Addmod_npc_banterScripts()
{
    LOG_INFO("server.loading", "[NpcBanter] Registering mod-npc-banter scripts.");

    new NpcBanterConfigWorldScript();
    new NpcBanterCreatureScript();
    new NpcBanterWorldScript();
}
