-- mod-nemesis: removes everything from the characters database.
DROP TABLE IF EXISTS `mod_nemesis_hunt`;
DROP TABLE IF EXISTS `mod_nemesis_event`;
DROP TABLE IF EXISTS `mod_nemesis_luck`;
DROP TABLE IF EXISTS `mod_nemesis_champion`;
DELETE FROM `updates` WHERE `name` LIKE 'mod\_nemesis\_%';
