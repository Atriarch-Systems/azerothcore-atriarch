/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PassLeadershipToMasterAction.h"

#include "Event.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotOperations.h"
#include "PlayerbotWorldThreadProcessor.h"

bool PassLeadershipToMasterAction::Execute(Event /*event*/)
{
    if (Player* master = GetMaster())
        if (master && master != bot && bot->GetGroup() && bot->GetGroup()->IsMember(master->GetGUID()))
        {
            auto setLeaderOp = std::make_unique<GroupSetLeaderOperation>(bot->GetGUID(), master->GetGUID());
            PlayerbotWorldThreadProcessor::instance().QueueOperation(std::move(setLeaderOp));

            if (!message.empty())
                botAI->TellMasterNoFacing(message);

            if (sRandomPlayerbotMgr.IsRandomBot(bot))
            {
                botAI->ResetStrategies();
                botAI->Reset();
            }

            return true;
        }

    return false;
}

bool PassLeadershipToMasterAction::isUseful()
{
    return botAI->IsAlt() && bot->GetGroup() && bot->GetGroup()->IsLeader(bot->GetGUID());
}

bool GiveLeaderAction::isUseful()
{
    // AiPlayerbot.Lfg.AutoLeaderHandoff (default off): without it, a bot that ends up leading an
    // LFG-formed group (e.g. by the dungeon finder's formation coin flip) simply keeps leading --
    // no automatic hand-off to a real player master just because they share the instance. See
    // docs/dungeon-leadership-and-summon.md, section 1.
    return sPlayerbotAIConfig.lfgAutoLeaderHandoff && botAI->HasActivePlayerMaster() && bot->GetGroup() &&
           bot->GetGroup()->IsLeader(bot->GetGUID());
}
