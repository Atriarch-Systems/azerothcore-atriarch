-- Town NPC Banter pilot list (docs/npc-banter.md "Known v1 limitation"): the
-- feature does nothing with NpcBanter.Enable=1 alone until real creature.guid
-- rows exist here. Hand-curated, verified against the real base-DB spawns in
-- data/sql/base/db_world/creature.sql / creature_template.sql - a small
-- cross-faction pilot (Stormwind + Orgrimmar) rather than an exhaustive list,
-- per the design doc's own "~20-40 GUIDs, explicit named list" philosophy.

INSERT INTO `mod_npc_banter_config` (`guid`, `entry`, `enabled`, `archetype_key`) VALUES
-- Stormwind City Guard (entry 68), Trade District / Old Town spawns
(189,   68,  1, 'GUARD'),
(190,   68,  1, 'GUARD'),
(2473,  68,  1, 'GUARD'),
-- Innkeeper Farley (entry 295), Stormwind Trade District inn
(80346, 295, 1, 'INNKEEPER'),
-- Orgrimmar Grunt (entry 3296), Valley of Strength / Valley of Honor spawns
(3382,  3296, 1, 'GUARD'),
(3383,  3296, 1, 'GUARD'),
(3384,  3296, 1, 'GUARD')
ON DUPLICATE KEY UPDATE
  `entry` = VALUES(`entry`),
  `enabled` = VALUES(`enabled`),
  `archetype_key` = VALUES(`archetype_key`);
