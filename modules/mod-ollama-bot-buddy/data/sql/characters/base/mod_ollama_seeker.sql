-- Seeker experiment tables (docs/seeker-selenwe.md). Must stay idempotent:
-- base files may be re-applied when their hash drifts.
CREATE TABLE IF NOT EXISTS `mod_ollama_seeker_memory` (
  `guid` BIGINT UNSIGNED NOT NULL,
  `state` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `notes` TEXT,
  `updated` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Journey journal: every notable event in the seeker's life (directives,
-- level-ups, zone changes, deaths, quests, sightings, conversations).
-- `consolidated` rows have been folded into a chronicle chapter.
CREATE TABLE IF NOT EXISTS `mod_ollama_seeker_journal` (
  `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `guid` BIGINT UNSIGNED NOT NULL,
  `at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `event_type` VARCHAR(24) NOT NULL DEFAULT 'directive',
  `directive` VARCHAR(32) NOT NULL DEFAULT '',
  `target` VARCHAR(64) NOT NULL DEFAULT '',
  `say` VARCHAR(255) NOT NULL DEFAULT '',
  `consolidated` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY `idx_guid_at` (`guid`, `at`),
  KEY `idx_guid_consolidated` (`guid`, `consolidated`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Chronicle: LLM-consolidated first-person memoir chapters built from the
-- journal (docs/seeker-selenwe.md, "journey memory").
CREATE TABLE IF NOT EXISTS `mod_ollama_seeker_chronicle` (
  `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `guid` BIGINT UNSIGNED NOT NULL,
  `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `chapter_text` TEXT NOT NULL,
  PRIMARY KEY (`id`),
  KEY `idx_guid_id` (`guid`, `id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
