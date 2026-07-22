/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_REMEMBERTAXIACTION_H
#define PLAYERBOTS_REMEMBERTAXIACTION_H

#include "Action.h"
#include "ObjectGuid.h"
#include <vector>

class PlayerbotAI;

class RememberTaxiAction : public Action
{
public:
    RememberTaxiAction(PlayerbotAI* botAI) : Action(botAI, "remember taxi") {}

    bool Execute(Event event) override;

    // Populates a follower bot's own "last taxi" LastMovement value directly with the same two
    // fields Execute() fills in from a real CMSG_ACTIVATETAXI/CMSG_ACTIVATETAXIEXPRESS packet
    // (taxiMaster, taxiNodes). Virtual (bot) group leaders - e.g. mod-ollama-bot-buddy's guild
    // dungeon runs - never send that packet, so masterIncomingPacketHandlers has nothing to
    // hand a follower's RememberTaxiAction::Execute() in the first place; this is the entry
    // point an orchestrator calls instead so TaxiAction's staggered follower-takeoff scheduler
    // still fires. Restricted to ManagedBotRegistry-recognized bots/groups (ManagedBotRegistry.h)
    // - a no-op for anyone else, so it cannot leak into ordinary real-player-led play. This is a
    // new entry point, not a change to Execute()'s existing packet-driven behavior above.
    static void RememberManagedTaxi(PlayerbotAI* followerAI, ObjectGuid taxiMaster, std::vector<uint32> const& taxiNodes);
};

#endif
