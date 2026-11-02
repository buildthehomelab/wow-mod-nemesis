-- mod-nemesis: removes everything from the world database.
DELETE FROM `creature_template_model` WHERE `CreatureID` BETWEEN 9600000 AND 9601999;
DELETE FROM `creature_equip_template` WHERE `CreatureID` BETWEEN 9600000 AND 9601999;
DELETE FROM `creature_template` WHERE `entry` BETWEEN 9600000 AND 9601999;
DROP TABLE IF EXISTS `mod_nemesis_creature`;
DELETE FROM `updates` WHERE `name` LIKE 'mod\_nemesis\_%';
