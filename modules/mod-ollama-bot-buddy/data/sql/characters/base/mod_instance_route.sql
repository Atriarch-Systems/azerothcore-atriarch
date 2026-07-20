-- SUPERSEDED: route data now lives in the WORLD db
-- (mod_guild_dungeon_route, see data/sql/db-world/base/mod_guild_dungeon.sql).
-- This file is retained only so the table keeps existing on servers that
-- already applied it; nothing reads it any more.
-- Instance route data for the intent layer (scripted progression, not LLM).
-- Must stay idempotent: base files may be re-applied when their hash drifts.
--
-- kind: 'travel' = move here and continue; 'boss' = an encounter is expected
-- here (the group pauses for combat, then advances once out of combat).
--
-- HOW THIS DATA WAS DERIVED (repeat for other instances):
--   Boss coordinates come from the world DB, e.g. for Naxxramas (map 533):
--     SELECT ct.entry, ct.name,
--            ROUND(c.position_x,2), ROUND(c.position_y,2), ROUND(c.position_z,2)
--     FROM creature c
--     JOIN creature_template ct ON ct.entry = c.id
--     WHERE c.map = 533 AND ct.rank = 3        -- rank 3 = world boss
--     ORDER BY ct.name;
--   Entrance/hub coordinates come from areatrigger_teleport
--   (SELECT target_position_* FROM areatrigger_teleport WHERE target_map = 533).
--   Step ORDER IS AUTHORED, not auto-generated: the rows below follow the
--   Spider wing as a player would walk it (entrance -> hub -> wing entry ->
--   Anub'Rekhan -> Grand Widow Faerlina), with travel waypoints inserted
--   between bosses so the path stays inside the corridors. A pure
--   nearest-neighbour ordering over boss rows alone is NOT safe: it hops
--   between wings.
CREATE TABLE IF NOT EXISTS `mod_instance_route` (
  `map_id` INT UNSIGNED NOT NULL,
  `step` INT UNSIGNED NOT NULL,
  `x` FLOAT NOT NULL,
  `y` FLOAT NOT NULL,
  `z` FLOAT NOT NULL,
  `label` VARCHAR(64) NOT NULL DEFAULT '',
  `kind` VARCHAR(16) NOT NULL DEFAULT 'travel',
  PRIMARY KEY (`map_id`, `step`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Naxxramas (533) - Spider wing, entrance through Grand Widow Faerlina.
-- (Seed rows are upserted; the table is shared with routes for other maps.)
INSERT INTO `mod_instance_route` (`map_id`, `step`, `x`, `y`, `z`, `label`, `kind`) VALUES
  (533, 1, 3005.68, -3435.11, 293.88, 'Entrance hub',            'travel'),
  (533, 2, 3111.00, -3435.00, 293.30, 'Spider wing corridor',    'travel'),
  (533, 3, 3239.00, -3441.00, 290.00, 'Spider wing approach',    'travel'),
  (533, 4, 3308.59, -3476.29, 287.16, 'Anub''Rekhan',            'boss'),
  (533, 5, 3345.00, -3546.00, 275.00, 'Faerlina approach',       'travel'),
  (533, 6, 3353.25, -3620.10, 261.08, 'Grand Widow Faerlina',    'boss')
ON DUPLICATE KEY UPDATE
  `x` = VALUES(`x`), `y` = VALUES(`y`), `z` = VALUES(`z`),
  `label` = VALUES(`label`), `kind` = VALUES(`kind`);
