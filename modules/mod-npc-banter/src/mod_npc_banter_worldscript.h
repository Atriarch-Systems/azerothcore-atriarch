#ifndef MOD_NPC_BANTER_WORLDSCRIPT_H
#define MOD_NPC_BANTER_WORLDSCRIPT_H

#include "ScriptMgr.h"

// Single WorldScript::OnUpdate accumulator: drains async LLM results produced
// by earlier ticks first, then scans only the small NpcBanterRegistry
// allowlist (never all creatures, never all players) every NpcBanter.TickSeconds.
class NpcBanterWorldScript : public WorldScript
{
public:
    NpcBanterWorldScript();
    void OnUpdate(uint32 diff) override;
};

#endif // MOD_NPC_BANTER_WORLDSCRIPT_H
