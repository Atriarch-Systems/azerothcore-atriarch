/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TRIGGERINTERVAL_H
#define PLAYERBOTS_TRIGGERINTERVAL_H

#include "Config.h"

// Kill switch for relaxed trigger/value check intervals. Interval semantics match
// Trigger/CalculatedValue: 1 = every tick, 2-99 = seconds, >= 100 = milliseconds.
inline int RelaxedTriggerInterval(int relaxed, int original = 1)
{
    static bool const enabled = sConfigMgr->GetOption<bool>("AiPlayerbot.RelaxedTriggerIntervals", true);
    return enabled ? relaxed : original;
}

#endif
