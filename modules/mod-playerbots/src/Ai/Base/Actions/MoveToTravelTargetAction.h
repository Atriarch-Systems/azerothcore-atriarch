/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MOVETOTRAVELTARGETACTION_H
#define PLAYERBOTS_MOVETOTRAVELTARGETACTION_H

#include "MovementActions.h"

class PlayerbotAI;

class MoveToTravelTargetAction : public MovementAction
{
public:
    MoveToTravelTargetAction(PlayerbotAI* botAI) : MovementAction(botAI, "move to travel target") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    // Flight-before-teleport one-shot latch (docs/playerbot-realistic-travel.md, step 4),
    // the per-action twin of NewRpgInfo's MOVE_FAR latch: when a max-retry fire commits a
    // travel destination to a taxi ride, remember that destination and the chosen
    // flightmaster so arrival at the flightmaster actually boards, and so a later
    // max-retry fire for the same destination skips the flight attempt and falls through
    // to the retry-cooldown backstop. Keyed by destination, so a changed travel target
    // naturally invalidates the latch.
    WorldPosition flightAttemptDest;
    WorldPosition flightAttemptFmPos;
};

#endif
