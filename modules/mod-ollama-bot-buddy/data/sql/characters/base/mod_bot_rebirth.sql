-- Rebirth cycling state (population pyramid). Must stay idempotent:
-- base files may be re-applied when their hash drifts.
-- guid 0 is a sentinel row anchoring the global schedule on first run.
CREATE TABLE IF NOT EXISTS `mod_bot_rebirth` (
  `guid` INT UNSIGNED NOT NULL,
  `last_rebirth` BIGINT UNSIGNED NOT NULL DEFAULT 0,
  `rebirth_count` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`),
  KEY `idx_last_rebirth` (`last_rebirth`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
