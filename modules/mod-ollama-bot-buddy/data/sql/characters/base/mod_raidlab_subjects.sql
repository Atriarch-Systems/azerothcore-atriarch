-- Raid lab subject roster. Must stay idempotent: base files may be
-- re-applied when their hash drifts.
CREATE TABLE IF NOT EXISTS `mod_raidlab_subjects` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `guid` INT UNSIGNED NOT NULL,
  `name` VARCHAR(48) NOT NULL DEFAULT '',
  `role` VARCHAR(16) NOT NULL DEFAULT 'dps',
  PRIMARY KEY (`id`),
  KEY `idx_guid` (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
