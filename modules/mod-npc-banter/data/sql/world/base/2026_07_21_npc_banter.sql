-- Town NPC Banter (docs/npc-banter.md): opt-in list of world NPCs that may
-- speak LLM-flavored ambient barks, plus the small set of job-archetype
-- persona prompts they draw from.
--
-- WORLD DB (not characters): creature.guid/creature_template.entry live here,
-- and these rows are hand-curated static configuration, not per-character
-- state. Must stay idempotent: base files may be re-applied when their hash
-- drifts.

-- --------------------------------------------------------------------------
-- Eligibility: explicit per-spawn opt-in, keyed by creature.guid (the
-- specific spawn, not the shared creature_template.entry - two spawns of the
-- same "Stormwind City Guard" template may want different backstories).
--
-- `entry` is validated against the live creature's entry at registration time
-- (see mod_npc_banter_registry.cpp) rather than trusted blindly, so a stale
-- row left behind after a GM deletes/re-adds a spawn is skipped with a
-- warning instead of silently attaching an old archetype/backstory to an
-- unrelated NPC.
--
-- v1 ships with no seed rows here - hand-curate ~20-40 guards/vendors/
-- innkeepers per capital by inserting into this table directly, the same
-- explicit-named-list philosophy docs/director-mode.md uses for
-- OllamaBotControl.BotNames. Example:
--   INSERT INTO `mod_npc_banter_config` (`guid`, `entry`, `archetype_key`, `backstory`)
--     VALUES (12345, 68, 'GUARD', 'You have guarded the Old Town gate for a decade.');
-- --------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `mod_npc_banter_config` (
  `guid`          INT UNSIGNED NOT NULL,                    -- creature.guid (specific spawn)
  `entry`         INT UNSIGNED NOT NULL,                    -- creature_template.entry, validated at registration
  `enabled`       TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `archetype_key` VARCHAR(64) NOT NULL DEFAULT 'GUARD',
  `backstory`     TEXT,                                     -- optional per-spawn flavor override
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- --------------------------------------------------------------------------
-- Archetype persona prompts. Terse and non-conversational by design so a
-- guard doesn't ramble into an out-of-character monologue.
-- --------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `mod_npc_banter_archetypes` (
  `key`    VARCHAR(64) NOT NULL,
  `prompt` TEXT NOT NULL,
  PRIMARY KEY (`key`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT INTO `mod_npc_banter_archetypes` (`key`, `prompt`) VALUES
('GUARD',     'A stern city guard. Terse, watchful, suspicious of strangers. Never breaks composure.'),
('VENDOR',    'A practical shopkeeper. Talks about wares, prices, and the day''s trade.'),
('INNKEEPER', 'A warm, tired innkeeper. Talks about travelers, rumors, and a good night''s rest.')
ON DUPLICATE KEY UPDATE
  `prompt` = VALUES(`prompt`);
