-- Guild dungeon progression: catalog + route reference data.
-- WORLD DB (not characters): these are hand-authored static reference rows and
-- must survive a character-DB wipe on a fresh-genesis realm.
-- Must stay idempotent: base files may be re-applied when their hash drifts.
--
-- HOW THIS DATA WAS DERIVED (repeat for every dungeon added):
--   Entrance:  SELECT target_map, target_position_x, target_position_y,
--                     target_position_z, target_orientation
--              FROM areatrigger_teleport WHERE target_map = <mapId>;
--   Bosses:    SELECT ct.entry, ct.name, c.position_x, c.position_y, c.position_z
--              FROM creature c JOIN creature_template ct ON ct.entry = c.id
--              WHERE c.map = <mapId> AND ct.entry IN (<known boss entries>);
--   NOTE: creature_template.rank = 3 is WORLD BOSS only and returns 0 rows for
--   5-mans; flags_extra & 0x10000000 (CREATURE_FLAG_EXTRA_DUNGEON_BOSS) is also
--   unset for classic dungeons in this DB. Boss entries are therefore authored
--   per dungeon and their coordinates read back from `creature`.
--   Step ORDER IS AUTHORED - nearest-neighbour ordering walks through walls.

CREATE TABLE IF NOT EXISTS `mod_guild_dungeon_catalog` (
  `dungeon_id`   INT UNSIGNED NOT NULL,
  `route_key`    VARCHAR(32) NOT NULL,           -- routes key off this, so SM wings can share map 189
  `map_id`       INT UNSIGNED NOT NULL,
  `difficulty`   TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `name`         VARCHAR(64) NOT NULL,
  `team`         TINYINT NOT NULL DEFAULT 2,     -- 0=alliance 1=horde 2=both
  `min_level`    TINYINT UNSIGNED NOT NULL,
  `max_level`    TINYINT UNSIGNED NOT NULL,
  `entrance_map` INT UNSIGNED NOT NULL,
  `entrance_x`   FLOAT NOT NULL,
  `entrance_y`   FLOAT NOT NULL,
  `entrance_z`   FLOAT NOT NULL,
  `entrance_o`   FLOAT NOT NULL DEFAULT 0,
  `rally_map`    INT UNSIGNED NOT NULL DEFAULT 0, -- town/city the group forms up in
  `rally_x`      FLOAT NOT NULL DEFAULT 0,
  `rally_y`      FLOAT NOT NULL DEFAULT 0,
  `rally_z`      FLOAT NOT NULL DEFAULT 0,
  `rally_name`   VARCHAR(64) NOT NULL DEFAULT '',
  `group_size`   TINYINT UNSIGNED NOT NULL DEFAULT 5,
  `tier`         TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `enabled`      TINYINT UNSIGNED NOT NULL DEFAULT 1,
  PRIMARY KEY (`dungeon_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `mod_guild_dungeon_route` (
  `route_key`  VARCHAR(32) NOT NULL,
  `step`       INT UNSIGNED NOT NULL,
  `x`          FLOAT NOT NULL,
  `y`          FLOAT NOT NULL,
  `z`          FLOAT NOT NULL,
  `label`      VARCHAR(64) NOT NULL DEFAULT '',
  `kind`       VARCHAR(16) NOT NULL DEFAULT 'travel',  -- travel | boss
  `radius`     FLOAT NOT NULL DEFAULT 0,               -- 0 = use Intent.AdvanceRadius
  `boss_entry` INT UNSIGNED NOT NULL DEFAULT 0,        -- creature entry for the kill test
  `boss_index` INT NOT NULL DEFAULT -1,                -- InstanceScript boss id, -1 = proximity test
  PRIMARY KEY (`route_key`, `step`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ---------------------------------------------------------------------------
-- Phase 1 seed: The Deadmines (map 36, Alliance, 15-25).
-- Entrance from areatrigger_teleport (id 78). Rally point: Sentinel Hill,
-- Westfall - the closest town with a flight master to the instance portal.
-- ---------------------------------------------------------------------------
INSERT INTO `mod_guild_dungeon_catalog`
  (`dungeon_id`, `route_key`, `map_id`, `difficulty`, `name`, `team`,
   `min_level`, `max_level`, `entrance_map`, `entrance_x`, `entrance_y`, `entrance_z`, `entrance_o`,
   `rally_map`, `rally_x`, `rally_y`, `rally_z`, `rally_name`,
   `group_size`, `tier`, `enabled`)
VALUES
  (36, 'deadmines', 36, 0, 'The Deadmines', 0,
   15, 25, 36, -16.40, -383.07, 61.78, 1.86,
   0, -10510.0, 1035.0, 55.0, 'Sentinel Hill, Westfall',
   5, 1, 1)
ON DUPLICATE KEY UPDATE
  `route_key` = VALUES(`route_key`), `map_id` = VALUES(`map_id`), `name` = VALUES(`name`),
  `team` = VALUES(`team`), `min_level` = VALUES(`min_level`), `max_level` = VALUES(`max_level`),
  `entrance_map` = VALUES(`entrance_map`), `entrance_x` = VALUES(`entrance_x`),
  `entrance_y` = VALUES(`entrance_y`), `entrance_z` = VALUES(`entrance_z`),
  `entrance_o` = VALUES(`entrance_o`), `rally_map` = VALUES(`rally_map`),
  `rally_x` = VALUES(`rally_x`), `rally_y` = VALUES(`rally_y`), `rally_z` = VALUES(`rally_z`),
  `rally_name` = VALUES(`rally_name`), `group_size` = VALUES(`group_size`),
  `tier` = VALUES(`tier`), `enabled` = VALUES(`enabled`);

-- Deadmines route. Boss coordinates read from `creature` (entries 644 Rhahk'Zor,
-- 3586 Miner Johnson, 646 Mr. Smite, 647 Captain Greenskin, 639 Edwin VanCleef,
-- 645 Cookie); travel waypoints authored between them along the corridor route.
INSERT INTO `mod_guild_dungeon_route`
  (`route_key`, `step`, `x`, `y`, `z`, `label`, `kind`, `radius`, `boss_entry`, `boss_index`)
VALUES
  ('deadmines',  1,  -16.40, -383.07,  61.78, 'Instance entrance',      'travel',  0,    0, -1),
  ('deadmines',  2, -100.00, -400.00,  58.00, 'Upper tunnel',           'travel',  0,    0, -1),
  ('deadmines',  3, -170.00, -430.00,  55.00, 'Rhahk''Zor approach',    'travel',  0,    0, -1),
  ('deadmines',  4, -192.90, -448.20,  54.40, 'Rhahk''Zor',             'boss',   12,  644, -1),
  ('deadmines',  5, -180.00, -490.00,  53.50, 'Mine descent',           'travel',  0,    0, -1),
  ('deadmines',  6, -152.50, -526.40,  51.10, 'Miner Johnson',          'boss',   14, 3586, -1),
  ('deadmines',  7, -190.00, -570.00,  50.00, 'Lower mine',             'travel',  0,    0, -1),
  ('deadmines',  8, -130.80, -605.50,  15.20, 'Ironclad approach',      'travel',  0,    0, -1),
  ('deadmines',  9,  -80.00, -790.00,  40.00, 'Foundry floor',          'travel',  0,    0, -1),
  ('deadmines', 10,  -22.80, -797.30,  20.40, 'Mr. Smite',              'boss',   14,  646, -1),
  ('deadmines', 11,  -59.60, -820.10,  41.60, 'Captain Greenskin',      'boss',   14,  647, -1),
  ('deadmines', 12,  -87.40, -819.90,  39.30, 'Edwin VanCleef',         'boss',   16,  639, -1),
  ('deadmines', 13,  -67.60, -853.70,  17.10, 'Cookie',                 'boss',   14,  645, -1)
ON DUPLICATE KEY UPDATE
  `x` = VALUES(`x`), `y` = VALUES(`y`), `z` = VALUES(`z`), `label` = VALUES(`label`),
  `kind` = VALUES(`kind`), `radius` = VALUES(`radius`),
  `boss_entry` = VALUES(`boss_entry`), `boss_index` = VALUES(`boss_index`);

-- ---------------------------------------------------------------------------
-- Naxxramas (map 533) Spider wing - migrated from the old characters-DB
-- mod_instance_route table so `.raidlab` keeps working against the world-DB
-- route store. Boss coordinates from `creature` (rank 3 world bosses).
-- ---------------------------------------------------------------------------
INSERT INTO `mod_guild_dungeon_route`
  (`route_key`, `step`, `x`, `y`, `z`, `label`, `kind`, `radius`, `boss_entry`, `boss_index`)
VALUES
  ('naxx', 1, 3005.68, -3435.11, 293.88, 'Entrance hub',         'travel',  0,     0, -1),
  ('naxx', 2, 3111.00, -3435.00, 293.30, 'Spider wing corridor', 'travel',  0,     0, -1),
  ('naxx', 3, 3239.00, -3441.00, 290.00, 'Spider wing approach', 'travel',  0,     0, -1),
  ('naxx', 4, 3308.59, -3476.29, 287.16, 'Anub''Rekhan',         'boss',   18, 15956, -1),
  ('naxx', 5, 3345.00, -3546.00, 275.00, 'Faerlina approach',    'travel',  0,     0, -1),
  ('naxx', 6, 3353.25, -3620.10, 261.08, 'Grand Widow Faerlina', 'boss',   18, 15953, -1)
ON DUPLICATE KEY UPDATE
  `x` = VALUES(`x`), `y` = VALUES(`y`), `z` = VALUES(`z`), `label` = VALUES(`label`),
  `kind` = VALUES(`kind`), `radius` = VALUES(`radius`),
  `boss_entry` = VALUES(`boss_entry`), `boss_index` = VALUES(`boss_index`);
