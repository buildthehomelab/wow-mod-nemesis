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

-- Hunts made before these existed get them here (MySQL has no ADD COLUMN IF NOT EXISTS).
SET @nemesis_kills := (SELECT COUNT(*) FROM information_schema.columns
    WHERE table_schema = DATABASE() AND table_name = 'mod_nemesis_hunt' AND column_name = 'kills');
SET @nemesis_sql := IF(@nemesis_kills = 0,
    'ALTER TABLE `mod_nemesis_hunt` ADD COLUMN `kills` int unsigned NOT NULL DEFAULT 0, ADD COLUMN `lesson` tinyint unsigned NOT NULL DEFAULT 0, ADD COLUMN `learned` int unsigned NOT NULL DEFAULT 0, ADD COLUMN `name` varchar(100) NOT NULL DEFAULT \'\'',
    'DO 0');
PREPARE nemesis_stmt FROM @nemesis_sql;
EXECUTE nemesis_stmt;
DEALLOCATE PREPARE nemesis_stmt;

CREATE TABLE IF NOT EXISTS `mod_nemesis_event` (
  `id` int unsigned NOT NULL AUTO_INCREMENT,
  `at` int unsigned NOT NULL,
  `guid` int unsigned NOT NULL COMMENT 'character hunted',
  `event` tinyint unsigned NOT NULL COMMENT '1 started, 2 appeared, 3 killed, 4 finished, 5 it won, 6 someone else killed it, 7 mount, 8 learned',
  `rare_entry` int unsigned NOT NULL,
  `rank` tinyint unsigned NOT NULL,
  `streak` int unsigned NOT NULL DEFAULT 0 COMMENT 'kills in this hunt',
  `player_level` tinyint unsigned NOT NULL DEFAULT 0,
  `bots` tinyint unsigned NOT NULL DEFAULT 0 COMMENT 'a bot had a part in the fight',
  `seconds` int unsigned NOT NULL DEFAULT 0 COMMENT 'length of the fight',
  `affixes` int unsigned NOT NULL DEFAULT 0,
  `name` varchar(100) NOT NULL DEFAULT '' COMMENT 'the nemesis, as it was called then',
  PRIMARY KEY (`id`),
  KEY `event` (`event`, `streak`),
  KEY `guid` (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-nemesis: what happened to each hunt';

CREATE TABLE IF NOT EXISTS `mod_nemesis_luck` (
  `account` int unsigned NOT NULL,
  `misses` int unsigned NOT NULL DEFAULT 0 COMMENT 'mount rolls missed since the last win',
  PRIMARY KEY (`account`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-nemesis: bad luck protection for the mount';

CREATE TABLE IF NOT EXISTS `mod_nemesis_champion` (
  `week_end` int unsigned NOT NULL COMMENT 'when the week ended',
  `guid` int unsigned NOT NULL DEFAULT 0 COMMENT 'the character who won it, 0 for nobody',
  `name` varchar(12) NOT NULL DEFAULT '' COMMENT 'as it was called then',
  `kills` int unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`week_end`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-nemesis: the weekly champions';
