-- DB update 2026_07_11_00 -> 2026_07_21_00
-- Allow weapon skill-ups from striking Training Dummies, same as striking a live creature.
-- Player::UpdateWeaponSkill (PlayerUpdates.cpp:993) skips the skill-up entirely when the victim
-- creature has CREATURE_FLAG_EXTRA_NO_SKILL_GAINS (0x00040000) set in flags_extra. All AIName =
-- 'npc_training_dummy' rows in the base DB ship with exactly that flag. Clear only that bit
-- (bitwise AND, not an overwrite) so any other extra flags on these rows are left untouched.

UPDATE `creature_template` SET `flags_extra` = `flags_extra` & ~0x00040000
WHERE `entry` IN (17578, 24792, 30527, 31143, 31144, 31146, 32541, 32542, 32543, 32545, 32546, 32666, 32667);
