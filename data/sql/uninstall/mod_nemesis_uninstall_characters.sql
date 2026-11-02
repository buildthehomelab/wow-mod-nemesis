-- mod-nemesis: removes everything from the characters database.
DROP TABLE IF EXISTS `mod_nemesis_hunt`;
DELETE FROM `updates` WHERE `name` LIKE 'mod\_nemesis\_%';
