/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LfgBootVoteAction.h"

#include "Config.h"
#include "Event.h"
#include "Opcodes.h"
#include "Playerbots.h"
#include "WorldPacket.h"

bool LfgBootVoteAction::Execute(Event event)
{
    // Inline config read cached in a function-local static; a later consolidation pass will move
    // this into PlayerbotAIConfig.
    static bool const autoBootVote = sConfigMgr->GetOption<bool>("AiPlayerbot.Lfg.AutoBootVote", true);
    if (!autoBootVote)
        return false;

    if (event.getPacket().empty())
        return false;

    // SMSG_LFG_BOOT_PROPOSAL_UPDATE layout, per WorldSession::SendLfgBootProposalUpdate
    // (LFGHandler.cpp): inProgress, didVote, myVote, victim guid, total votes, agree count,
    // seconds left, votes needed, reason. Only the first four fields matter here.
    WorldPacket p(event.getPacket());
    uint8 inProgress = 0;
    uint8 didVote = 0;
    uint8 myVote = 0;
    ObjectGuid victim;
    p >> inProgress >> didVote >> myVote >> victim;

    // The same packet is also broadcast when the vote resolves (inProgress = false) and after
    // each individual vote; only an open vote this bot has not answered needs a response. The
    // kicker and the victim are auto-voted by LFGMgr::InitBoot (AGREE/DENY respectively), so
    // didVote already covers both.
    if (!inProgress || didVote)
        return false;

    // Never vote on a boot targeting this bot. The victim's own packet arrives with didVote set
    // (auto-DENY) and LFGMgr::UpdateBoot ignores non-pending votes anyway, but keep an explicit
    // guard rather than rely on that.
    if (victim == bot->GetGUID())
        return false;

    // Vote YES the same way a real client would: queue CMSG_LFG_SET_BOOT_VOTE so that
    // WorldSession::HandleLfgSetBootVoteOpcode -> LFGMgr::UpdateBoot runs on the world thread,
    // mirroring how LfgAcceptAction answers proposals via CMSG_LFG_PROPOSAL_RESULT.
    WorldPacket* packet = new WorldPacket(CMSG_LFG_SET_BOOT_VOTE);
    *packet << true;
    bot->GetSession()->QueuePacket(packet);

    LOG_INFO("playerbots", "Bot {} {}:{} <{}>: votes to kick {} from LFG group",
             bot->GetGUID().ToString().c_str(), bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H",
             bot->GetLevel(), bot->GetName().c_str(), victim.ToString().c_str());
    return true;
}
