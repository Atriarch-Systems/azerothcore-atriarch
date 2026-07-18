-- Rumor sightings persistence (docs/seeker-selenwe.md). Must stay idempotent:
-- base files may be re-applied when their hash drifts.
CREATE TABLE IF NOT EXISTS `mod_ollama_chat_rumors` (
  `guid` BIGINT UNSIGNED NOT NULL,
  `player_name` VARCHAR(48) NOT NULL,
  `zone` VARCHAR(96) NOT NULL DEFAULT '',
  `epoch` BIGINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`, `player_name`),
  KEY `idx_epoch` (`epoch`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
