/*
 * mod-nemesis: a rare you killed comes back for you.
 *
 * Released under the MIT License.
 */

#ifndef MOD_NEMESIS_H
#define MOD_NEMESIS_H

#include "Define.h"
#include "ObjectGuid.h"

#include <array>
#include <string>
#include <vector>

class Creature;
class Player;
class Unit;

namespace Nemesis
{
    constexpr uint8 FINAL_RANK = 5;

    enum Affix : uint32
    {
        AFFIX_VAMPIRIC     = 0x01,  // heals for part of the damage it deals
        AFFIX_SWIFT        = 0x02,  // runs and attacks faster
        AFFIX_JUGGERNAUT   = 0x04,  // can't be stunned, rooted, feared, polymorphed or knocked back
        AFFIX_SAVAGE       = 0x08,  // hits harder
        AFFIX_SPELLWARD    = 0x10,  // takes less from spells
        AFFIX_ENRAGED      = 0x20,  // hits much harder when nearly dead
        AFFIX_REGENERATING = 0x40,  // heals over time, even in a fight
    };

    struct Config
    {
        bool enabled = true;
        float triggerChance = 5.0f;
        uint32 triggerMaxLevelsOver = 10;
        uint32 waitMinSeconds = 1800;
        uint32 waitMaxSeconds = 3600;
        uint32 lostTrailSeconds = 60;
        uint32 tickSeconds = 5;
        float speed = 16.0f;
        uint32 continentSeconds = 300;
        float continentYards = 400.0f;
        float warnYards = 300.0f;
        float appearYards = 80.0f;
        float spawnYards = 35.0f;
        float leashYards = 150.0f;
        float guardYards = 60.0f;
        std::vector<std::string> guardNames;
        std::array<float, FINAL_RANK> health{};         // ranks 1-4; the final rank has its own
        std::array<float, FINAL_RANK> damage{};
        std::array<float, 3> finalHealth{};             // by cap: 60, 70, 80
        std::array<float, 3> finalDamage{};
        uint32 holdLevels = 5;
        std::array<float, FINAL_RANK> scale{};
        std::array<std::vector<uint32>, FINAL_RANK> auras;
        uint32 goldPerLevel = 200;
        float mountChance = 25.0f;
        uint32 mountHumanoid = 0;
        uint32 mountUndead = 0;
        uint32 mountBeast = 0;
        uint32 mountDemon = 0;
        uint32 mountElemental = 0;
        uint32 mountMechanical = 0;
        uint32 mountOther = 0;
    };

    extern Config config;

    // What a creature needs to know when it appears.
    struct Fight
    {
        ObjectGuid owner;
        uint8 rank = 1;
        uint32 affixes = 0;
        uint8 level = 1;
    };

    void LoadConfig();
    void LoadData();        // at startup
    void ReloadCopies();    // on a command: a hunt under way keeps its creature

    bool IsBot(Player const* player);

    // The highest level this player can reach now, which the last rank fights at.
    uint8 FinalLevelFor(Player const* player);

    // From the creature's AI, on its map's thread.
    bool SetUpCreature(Creature* creature, Fight const& fight);
    void OnNemesisDied(Creature* creature, ObjectGuid owner, bool botAssisted);
    void OnNemesisWon(Creature* creature, Player* owner);
    void OnNemesisLostTrail(Creature* creature, ObjectGuid owner);

    // For commands, on the world thread.
    struct Status
    {
        bool found = false;
        uint32 rareEntry = 0;
        uint8 rank = 0;
        uint32 affixes = 0;
        uint32 waitSeconds = 0;
        bool engaged = false;
        std::string name;
    };

    Status StatusOf(Player const* player);
    std::string RankName(uint8 rank);
    std::string AffixNames(uint32 affixes);
    bool StartHunt(Player* player, uint32 rareEntry, uint8 rank);
    bool SetRank(Player* player, uint8 rank);
    bool HuntNow(Player* player);
    bool ClearHunt(Player* player);
}

void AddNemesisScripts();
void AddNemesisCreatureScripts();
void AddNemesisCommandScripts();

#endif
