-- mod-nemesis: what finishing the last rank earns. The server's copy of two rows of the client
-- patch (tools/build_patch.py): achievement 9510 in Feats of Strength and title 191 (bit 156), and
-- the weekly champion's title 192 (bit 157). The achievement has no criteria: the module grants it.
-- Without the patch rows the client shows none of them: Nemesis.Final.Achievement = 0 and
-- Nemesis.Champion.Title = 0 turn them off.

DELETE FROM `achievement_dbc` WHERE `ID` = 9510;
INSERT INTO `achievement_dbc` (`ID`, `Faction`, `Instance_Id`, `Supercedes`, `Title_Lang_enUS`, `Title_Lang_Mask`, `Description_Lang_enUS`, `Description_Lang_Mask`, `Category`, `Points`, `Ui_Order`, `Flags`, `IconID`, `Reward_Lang_enUS`, `Reward_Lang_Mask`, `Minimum_Criteria`, `Shares_Criteria`) VALUES
(9510, -1, -1, 0, 'Bane of the Undying', 16712190, 'Hunted by your nemesis, defeat it at its last rank.', 16712190, 81, 0, 1100, 0, 538, 'Title Reward: <Name>, Bane of the Undying', 16712190, 0, 0);

DELETE FROM `chartitles_dbc` WHERE `ID` IN (191, 192);
INSERT INTO `chartitles_dbc` (`ID`, `Condition_ID`, `Name_Lang_enUS`, `Name_Lang_Mask`, `Name1_Lang_enUS`, `Name1_Lang_Mask`, `Mask_ID`) VALUES
(191, 0, '%s, Bane of the Undying', 16712190, '%s, Bane of the Undying', 16712190, 156),
(192, 0, '%s the Nemesis Hunter', 16712190, '%s the Nemesis Hunter', 16712190, 157);

DELETE FROM `achievement_reward` WHERE `ID` = 9510;
INSERT INTO `achievement_reward` (`ID`, `TitleA`, `TitleH`, `ItemID`, `Sender`, `Subject`, `Body`, `MailTemplateID`) VALUES
(9510, 191, 191, 0, 0, NULL, NULL, 0);
