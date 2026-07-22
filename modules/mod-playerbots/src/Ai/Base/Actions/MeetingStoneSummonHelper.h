/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MEETINGSTONESUMMONHELPER_H
#define PLAYERBOTS_MEETINGSTONESUMMONHELPER_H

class GameObject;
class Player;

// Reusable helpers that drive the real, unmodified core meeting-stone summon mechanic: finding a
// real (type-23, GAMEOBJECT_TYPE_MEETINGSTONE) meeting stone near a bot, and using it to send a
// genuine SMSG_SUMMON_REQUEST (spell 23598) to a grouped player -- exactly as if a real player had
// right-clicked the stone. See docs/dungeon-leadership-and-summon.md, section 3a.
//
// Deliberately separate from SummonAction/UseMeetingStoneAction's Teleport() path in
// UseMeetingStoneAction.h/.cpp, which is an unrelated hand-rolled instant-teleport convenience used
// for a bot summoning itself to its master. That path is not reused or modified here.
namespace MeetingStoneSummonHelper
{
    // Scans for the nearest spawned, real meeting stone (GAMEOBJECT_TYPE_MEETINGSTONE) within
    // `range` yards of `bot`. Returns nullptr if none is found.
    GameObject* FindNearestRealMeetingStone(Player* bot, float range);

    // Drives the real core meeting-stone summon path: sets `bot`'s selection to `target` and calls
    // the real GameObject::Use(bot) on `meetingStone`, reproducing a player's right-click on the
    // stone byte for byte. This requires (matching GameObject.cpp's GAMEOBJECT_TYPE_MEETINGSTONE
    // case, which is what actually gets exercised):
    //   - `target` is in the same group as `bot` (the core checks IsInSameRaidWith(), which for a
    //     non-raid group just means "same Group*"),
    //   - both `bot` and `target` meet the stone's GameObjectTemplate::meetingstone.minLevel,
    //   - `target` is a valid, in-world player.
    // Returns false (and logs a debug line) with no side effects if any precondition fails. This
    // is a synchronous, single-tick call -- callers must still pass live, freshly-resolved
    // pointers, since this function does no guid re-resolution of its own.
    bool SummonPlayerViaMeetingStone(Player* bot, Player* target, GameObject* meetingStone);
}

#endif
