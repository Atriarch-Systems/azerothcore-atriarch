/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LFGBOOTVOTEACTION_H
#define PLAYERBOTS_LFGBOOTVOTEACTION_H

#include "Action.h"

class PlayerbotAI;

// Answers SMSG_LFG_BOOT_PROPOSAL_UPDATE (sent by LFGMgr::InitBoot to every group member when a
// vote-kick starts) with a YES vote. Without this, bots never answer boot votes at all: a kick
// needs LFG_GROUP_KICK_VOTES_NEEDED (3) agrees, so a real player in a bot group could never kick
// anyone, and each attempt locked the group in LFG_STATE_BOOT for the full 120s window. Wired the
// same way as "lfg proposal"/LfgAcceptAction: PlayerbotAI registers the packet handler,
// WorldPacketTriggerContext names the trigger, WorldPacketHandlerStrategy links it to this action.
// Config-gated by AiPlayerbot.Lfg.AutoBootVote (default true).
class LfgBootVoteAction : public Action
{
public:
    LfgBootVoteAction(PlayerbotAI* botAI) : Action(botAI, "lfg boot vote") {}

    bool Execute(Event event) override;
    bool isUseful() override { return true; }
};

#endif
