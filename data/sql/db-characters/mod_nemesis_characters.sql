-- mod-nemesis: each character's nemesis, if it has one.
CREATE TABLE IF NOT EXISTS `mod_nemesis_hunt` (
  `guid` int unsigned NOT NULL COMMENT 'character hunted',
  `rare_entry` int unsigned NOT NULL COMMENT 'creature_template of the rare it came from',
  `rank` tinyint unsigned NOT NULL DEFAULT 1,
  `affixes` int unsigned NOT NULL DEFAULT 0 COMMENT 'bits, see Nemesis.h',
  `wait_seconds` int unsigned NOT NULL DEFAULT 0 COMMENT 'time online left before it sets out again',
  `map` int unsigned NOT NULL DEFAULT 0 COMMENT 'where it is on its way to the character',
  `x` float NOT NULL DEFAULT 0,
  `y` float NOT NULL DEFAULT 0,
  `created_at` int unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-nemesis: active hunts';
