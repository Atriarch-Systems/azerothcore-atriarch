#pragma once
#include "ScriptMgr.h"

// Rebirth cycling (population pyramid): a deterministic cohort of random bots
// is periodically reborn at level 1 so the world always has a spread of
// levels, while the rest ("long-lived" bots) and all LLM-driven bots
// (OllamaBotControl.BotNames / SeekerName) are never touched.
//
// Config: Rebirth.Enable / Rebirth.PerDay / Rebirth.CohortSize
// (env: AC_REBIRTH_ENABLE / AC_REBIRTH_PERDAY / AC_REBIRTH_COHORTSIZE)
class BotRebirthLoop : public WorldScript
{
public:
    BotRebirthLoop();
    void OnStartup() override;
    void OnUpdate(uint32 diff) override;
};
