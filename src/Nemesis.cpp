/*
 * mod-nemesis: a rare you killed comes back for you.
 *
 *   how it starts   a rare dies in the open world; each real player credited with the kill rolls
 *                   Nemesis.TriggerChance. One nemesis per character. Bots and Hardcore
 *                   characters never get one: an ambush shouldn't end a Hardcore run.
 *   the hunt        no creature exists while it hunts. A hunt is a point on a map that moves
 *                   towards the player on the world thread, so distance is real and the cost is a
 *                   few sums per hunt. Another continent takes a fixed time. It stops close to a
 *                   player it can't reach (a capital, a town, guards nearby, a dungeon, in the air)
 *                   and appears as soon as it can. All of it runs on time the player is online.
 *                   The player is told where it's coming from as it closes in.
 *   the fight       a temporary summon (NemesisCreature.cpp). If the player runs too far or reaches
 *                   safety, the creature goes and the point carries on from where it stood.
 *   what it learns  how it lost decides the affix it comes back with (spells, crowd control, kiting,
 *                   out-healing it), and it says so when it arrives.
 *   how it ends     killed: gold, and it returns a rank stronger. Killed at the last rank: gone for
 *                   good, more gold, a Feat of Strength with a title, and a chance at a mount if
 *                   no bot had a part in it. Each missed roll raises the account's next one. It
 *                   kills the player: gone for good, nothing. Killed by someone else: back later,
 *                   at the same rank.
 *
 * What happens to a hunt goes into characters table mod_nemesis_event, for tuning and for a
 * leaderboard.
 *
 * Bots leave it alone through mod-rare-tracker, which goes by a creature's rare rank: the copies
 * have that rank, and that module needs no change for them. Without it, nothing stops a bot
 * attacking a nemesis.
 *
 * The world thread does the hunts in WorldScript::OnUpdate, when the maps are not updating, so it
 * may touch players and maps anywhere. Creature deaths come from map threads, hence the lock.
 *
 * Released under the MIT License.
 */

#include "Nemesis.h"
#include "AchievementMgr.h"
#include "CellImpl.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"
#include "Tokenize.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"

#if __has_include("ChallengesApi.h")
#include "ChallengesApi.h"
#define NEMESIS_HAS_CHALLENGES 1
#endif

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

using namespace Nemesis;

namespace Nemesis
{
    Config config;
}

namespace
{
    constexpr uint32 CORPSE_MS = 5 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 SPAWN_ANGLES = 8;
    constexpr uint32 SPAWN_TRIES_BEFORE_CLOSE = 3;
    constexpr float CLOSE_SPAWN_YARDS = 5.0f;
    constexpr float MAX_SPAWN_Z_DIFFERENCE = 12.0f;
    constexpr uint32 PROGRESSION_QUEST_BASE = 66000;
    constexpr uint32 PROGRESSION_TBC = 8;        // PROGRESSION_PRE_TBC in mod-individual-progression
    constexpr uint32 PROGRESSION_WOTLK = 13;     // PROGRESSION_TBC_TIER_5
    constexpr uint32 PROGRESSION_LAST = 18;

#ifdef NEMESIS_HAS_CHALLENGES
    constexpr uint16 CHALLENGE_HARDCORE = 1;
#endif

    // mod_nemesis_event.event
    enum HuntEvent : uint8
    {
        EVENT_STARTED = 1,      // a rare swore revenge
        EVENT_APPEARED = 2,     // it found the player
        EVENT_KILLED = 3,       // the player or their group killed it, and it will return
        EVENT_FINISHED = 4,     // the player or their group killed it at the last rank
        EVENT_WON = 5,          // it killed the player
        EVENT_STOLEN = 6,       // someone else killed it
        EVENT_MOUNT = 7,        // the player got the mount
        EVENT_LEARNED = 8,      // it took a lesson from its death; affixes are what it comes back with
    };

    struct Copies
    {
        uint32 hunter = 0;
        uint32 final = 0;
    };

    struct Hunt
    {
        uint32 guid = 0;
        uint32 rareEntry = 0;
        uint8 rank = 1;
        uint32 affixes = 0;
        uint32 waitSeconds = 0;
        uint32 map = 0;
        float x = 0.0f;
        float y = 0.0f;
        uint32 createdAt = 0;
        uint32 kills = 0;       // of this hunt: the streak
        uint8 lesson = LESSON_NONE;     // what it learned last, until it has said so
        uint32 learned = 0;     // affixes it took from a lesson, which stay

        // Not saved: a restart removes the creature, and the rest follows from where it stands.
        ObjectGuid creature;
        bool lastKnown = false;
        uint32 lastMap = 0;
        float lastX = 0.0f;
        float lastY = 0.0f;
        uint32 transitSeconds = 0;
        uint32 spawnFails = 0;
        uint8 warned = 0;       // 0 not yet, 1 from afar, 2 close
    };

    // Filled at startup and by a reload, both while the maps aren't updating.
    std::unordered_map<uint32, Copies> copies;

    // Hunts, by character. Recursive because a hunt step can despawn or summon a creature, and
    // nothing here should have to know whether that reaches back in.
    std::recursive_mutex huntsLock;
    std::unordered_map<uint32, Hunt> hunts;
    std::unordered_map<uint32, uint32> mountMisses;     // account -> rolls missed since the last win
    uint32 tickTimer = 0;

    // The four continents. Other maps of the same kind exist (the Deeprun Tram) and aren't for this.
    bool IsOpenWorld(uint32 mapId)
    {
        return mapId == 0 || mapId == 1 || mapId == 530 || mapId == 571;
    }

    void Tell(Player* player, std::string const& text)
    {
        if (WorldSession* session = player->GetSession())
            ChatHandler(session).PSendSysMessage("|cffff4040Nemesis:|r {}", text);
    }

    uint32 EntryOf(Hunt const& hunt);
    std::string NameOf(Hunt const& hunt);

    // Its name as the database wants it. Kept in the rows so a web page needs no world database.
    std::string EscapedNameOf(Hunt const& hunt)
    {
        std::string name = NameOf(hunt);
        CharacterDatabase.EscapeString(name);
        return name;
    }

    void SaveHunt(Hunt const& hunt)
    {
        CharacterDatabase.Execute("REPLACE INTO mod_nemesis_hunt (guid, rare_entry, `rank`, affixes, wait_seconds, map, x, y, created_at, kills, lesson, learned, name) "
            "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, '{}')",
            hunt.guid, hunt.rareEntry, uint32(hunt.rank), hunt.affixes, hunt.waitSeconds, hunt.map, hunt.x, hunt.y, hunt.createdAt,
            hunt.kills, uint32(hunt.lesson), hunt.learned, EscapedNameOf(hunt));
    }

    // Something that happened to a hunt, for tuning and for the leaderboard. Few enough to write each one.
    void RecordEvent(HuntEvent event, Hunt const& hunt, uint32 playerLevel, bool bots, uint32 seconds)
    {
        CharacterDatabase.Execute("INSERT INTO mod_nemesis_event (at, guid, event, rare_entry, `rank`, streak, player_level, bots, seconds, affixes, name) "
            "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, '{}')",
            uint32(GameTime::GetGameTime().count()), hunt.guid, uint32(event), hunt.rareEntry, uint32(hunt.rank), hunt.kills, playerLevel,
            bots ? 1 : 0, seconds, hunt.affixes, EscapedNameOf(hunt));

        LOG_INFO("module", "mod-nemesis: event {} character {} rare {} rank {} streak {} level {} bots {} seconds {} affixes {}",
            uint32(event), hunt.guid, hunt.rareEntry, uint32(hunt.rank), hunt.kills, playerLevel, bots ? 1 : 0, seconds, hunt.affixes);
    }

    // Where a point is from the player. In the world x runs north and y runs west.
    std::string DirectionFrom(Player const* player, float x, float y)
    {
        static char const* const names[] = { "north", "northwest", "west", "southwest", "south", "southeast", "east", "northeast" };

        float angle = std::atan2(y - player->GetPositionY(), x - player->GetPositionX());
        if (angle < 0.0f)
            angle += 2.0f * float(M_PI);

        return names[uint32(std::lround(angle / (float(M_PI) / 4.0f))) % std::size(names)];
    }

    // Words only the hunted player gets, in the middle of the screen, from something not there yet.
    void WhisperFrom(Player* player, std::string const& name, std::string const& text)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_RAID_BOSS_WHISPER, LANG_UNIVERSAL, ObjectGuid::Empty, player->GetGUID(), text, 0, name, player->GetName());
        player->SendDirectMessage(&data);
    }

    void Alarm(Player* player)
    {
        if (config.warnSound)
            player->PlayDirectSound(config.warnSound, player);
    }

    void DeleteHunt(uint32 guid)
    {
        CharacterDatabase.Execute("DELETE FROM mod_nemesis_hunt WHERE guid = {}", guid);
    }

    uint32 RandomWait()
    {
        return urand(config.waitMinSeconds, std::max(config.waitMinSeconds, config.waitMaxSeconds));
    }

    // Random affixes up to what the rank has, plus what it learned: never more than its rank in all.
    void RollAffixes(Hunt& hunt)
    {
        static uint32 const all[] = { AFFIX_VAMPIRIC, AFFIX_SWIFT, AFFIX_JUGGERNAUT, AFFIX_SAVAGE, AFFIX_SPELLWARD, AFFIX_ENRAGED, AFFIX_REGENERATING };
        uint32 wanted = hunt.rank >= FINAL_RANK ? 3 : hunt.rank >= 3 ? 2 : 1;

        auto count = [](uint32 bits)
        {
            uint32 result = 0;
            for (; bits; bits &= bits - 1)
                ++result;
            return result;
        };

        hunt.affixes |= hunt.learned;

        // Random ones make way for lessons. Each pass looks at the first random one left.
        for (uint32 affix : all)
            if (count(hunt.affixes) > uint32(hunt.rank) && (hunt.affixes & affix) && !(hunt.learned & affix))
                hunt.affixes &= ~affix;

        while (count(hunt.affixes) < wanted)
            hunt.affixes |= all[urand(0, std::size(all) - 1)];
    }

    uint32 EntryOf(Hunt const& hunt)
    {
        auto itr = copies.find(hunt.rareEntry);
        if (itr == copies.end())
            return 0;
        return hunt.rank >= FINAL_RANK ? itr->second.final : itr->second.hunter;
    }

    std::string NameOf(Hunt const& hunt)
    {
        CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(EntryOf(hunt));
        return proto ? proto->Name : "Your nemesis";
    }

    bool IsGuard(Creature const* creature, Player const* player)
    {
        if (creature->IsGuard())
            return true;

        // Plenty of guards lack the core's flag (Booty Bay Bruiser, Theramore Guard). The word
        // alone isn't enough either, Shattered Hand Guard is a monster.
        std::string const& name = creature->GetName();
        bool named = std::any_of(config.guardNames.begin(), config.guardNames.end(),
            [&name](std::string const& word) { return name.find(word) != std::string::npos; });
        if (!named)
            return false;

        if (creature->IsFriendlyTo(player))
            return true;

        FactionTemplateEntry const* faction = creature->GetFactionTemplateEntry();
        return faction && (faction->ourMask & (FACTION_MASK_ALLIANCE | FACTION_MASK_HORDE));
    }

    class GuardInRange
    {
    public:
        GuardInRange(WorldObject const* from, Player const* player, float range) : _from(from), _player(player), _range(range) { }

        bool operator()(Creature* creature) const
        {
            return creature->IsAlive() && _from->IsWithinDist(creature, _range, false) && IsGuard(creature, _player);
        }

    private:
        WorldObject const* _from;
        Player const* _player;
        float _range;
    };

    bool GuardsNearby(WorldObject const* from, Player const* player, float range)
    {
        Creature* found = nullptr;
        GuardInRange check(from, player, range);
        Acore::CreatureSearcher<GuardInRange> searcher(from, found, check);
        Cell::VisitObjects(from, searcher, range);
        return found != nullptr;
    }

    bool AreaIsSafe(uint32 areaId)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
        return area && (area->flags & (AREA_FLAG_CAPITAL | AREA_FLAG_SLAVE_CAPITAL | AREA_FLAG_SANCTUARY | AREA_FLAG_TOWN));
    }

    // A capital, a town or an inn. It stays out of all of them.
    bool IsSafePlace(Player* player)
    {
        if (player->IsInSanctuary() || player->HasRestFlag(REST_FLAG_IN_CITY) || player->HasRestFlag(REST_FLAG_IN_TAVERN)
            || player->HasRestFlag(REST_FLAG_IN_FACTION_AREA))
            return true;

        return AreaIsSafe(player->GetZoneId()) || AreaIsSafe(player->GetAreaId());
    }

    // Whether this is a moment it can appear in. Waiting costs it nothing: it stays close.
    bool CanAppear(Player* player)
    {
        if (!player->IsAlive() || player->IsGameMaster() || player->IsInCombat() || player->IsInFlight() || player->IsFlying()
            || player->IsFalling() || player->IsInWater() || player->GetVehicle() || player->GetTransport() || player->duel
            || player->HasStealthAura() || player->HasInvisibilityAura())   // it would lose them at once
            return false;

        // It appears some way from the player, and has to be clear of guards there too, or it
        // would leave again on the next tick.
        return !IsSafePlace(player) && !GuardsNearby(player, player, config.guardYards + config.spawnYards);
    }

    // A place it can run from to the player, behind them first.
    bool FindSpawnPoint(Player* player, float& x, float& y, float& z)
    {
        Map* map = player->GetMap();
        uint32 phase = player->GetPhaseMask();
        float playerZ = player->GetPositionZ();

        for (uint32 i = 0; i < SPAWN_ANGLES; ++i)
        {
            float angle = player->GetOrientation() + float(M_PI) + (float(i) * 2.0f * float(M_PI) / float(SPAWN_ANGLES));
            x = player->GetPositionX() + std::cos(angle) * config.spawnYards;
            y = player->GetPositionY() + std::sin(angle) * config.spawnYards;
            z = map->GetHeight(phase, x, y, playerZ + MAX_SPAWN_Z_DIFFERENCE, true, MAX_SPAWN_Z_DIFFERENCE * 2.0f);

            if (z <= INVALID_HEIGHT || std::fabs(z - playerZ) > MAX_SPAWN_Z_DIFFERENCE || map->IsInWater(phase, x, y, z, DEFAULT_COLLISION_HEIGHT))
                continue;

            PathGenerator path(player);
            path.CalculatePath(x, y, z, player->GetPositionX(), player->GetPositionY(), playerZ, false);
            if ((path.GetPathType() & PATHFIND_NORMAL) && !(path.GetPathType() & PATHFIND_NOPATH))
                return true;
        }

        return false;
    }

    // Puts the creature in the world and starts the fight.
    void Appear(Hunt& hunt, Player* player)
    {
        uint32 entry = EntryOf(hunt);
        if (!entry)
            return;

        float x, y, z;
        if (!FindSpawnPoint(player, x, y, z))
        {
            // Odd ground, a ledge, a narrow cave: try again, then come in close rather than never.
            if (++hunt.spawnFails < SPAWN_TRIES_BEFORE_CLOSE)
                return;
            player->GetNearPoint(player, x, y, z, 0.5f, CLOSE_SPAWN_YARDS, frand(0.0f, 2.0f * float(M_PI)));
        }
        hunt.spawnFails = 0;

        TempSummon* summon = player->SummonCreature(entry, x, y, z, player->GetAngle(x, y) + float(M_PI), TEMPSUMMON_CORPSE_TIMED_DESPAWN, CORPSE_MS);
        if (!summon)
            return;

        Fight fight;
        fight.owner = player->GetGUID();
        fight.rank = hunt.rank;
        fight.affixes = hunt.affixes;
        fight.level = hunt.rank >= FINAL_RANK ? FinalLevelFor(player) : player->GetLevel();
        fight.lesson = hunt.lesson;
        if (CreatureTemplate const* rare = sObjectMgr->GetCreatureTemplate(hunt.rareEntry))
            fight.caster = rare->unit_class == CLASS_MAGE;

        // Before the set up: a creature that kills fast still has to find this hunt its own.
        hunt.creature = summon->GetGUID();
        hunt.map = player->GetMapId();
        hunt.x = x;
        hunt.y = y;
        hunt.warned = 2;        // should it lose them, it doesn't announce itself all over again
        hunt.lesson = LESSON_NONE;
        SaveHunt(hunt);

        if (!SetUpCreature(summon, fight))
        {
            LOG_ERROR("module", "mod-nemesis: creature {} doesn't run script npc_mod_nemesis", entry);
            hunt.creature.Clear();
            summon->UnSummon();
            return;
        }

        if (WorldSession* session = player->GetSession())
            session->SendAreaTriggerMessage(Acore::StringFormat("{} has found you!", NameOf(hunt)));
        Alarm(player);
        Tell(player, Acore::StringFormat("{}, rank {} {} ({}), has found you!", NameOf(hunt), uint32(hunt.rank), RankName(hunt.rank), AffixNames(hunt.affixes)));
        RecordEvent(EVENT_APPEARED, hunt, player->GetLevel(), false, 0);
    }

    // The creature leaves the world and the hunt carries on from where it stood.
    void Disengage(Hunt& hunt, Creature* creature)
    {
        hunt.creature.Clear();
        hunt.map = creature->GetMapId();
        hunt.x = creature->GetPositionX();
        hunt.y = creature->GetPositionY();
        creature->DespawnOrUnsummon();
        SaveHunt(hunt);
    }

    void StepTowards(Hunt& hunt, float targetX, float targetY, float distance, float keepYards, float tick)
    {
        float travel = config.speed * tick;
        float along = std::min(travel, distance - keepYards);
        if (along <= 0.0f)
            return;

        hunt.x += (targetX - hunt.x) / distance * along;
        hunt.y += (targetY - hunt.y) / distance * along;
    }

    bool IsHardcore(ObjectGuid::LowType guid)
    {
#ifdef NEMESIS_HAS_CHALLENGES
        return (Challenges::Running(guid) & CHALLENGE_HARDCORE) != 0;
#else
        (void)guid;
        return false;
#endif
    }

    // Moves a hunt on. False when it's over.
    bool StepHunt(Hunt& hunt, uint32 tickSeconds)
    {
        Player* player = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(hunt.guid));
        bool online = player && player->IsInWorld() && !player->IsBeingTeleported();

        if (!hunt.creature.IsEmpty())
        {
            Map* map = sMapMgr->FindBaseMap(hunt.map);
            Creature* creature = map ? map->GetCreature(hunt.creature) : nullptr;
            if (!creature || !creature->IsAlive())
            {
                hunt.creature.Clear();
                hunt.waitSeconds = config.lostTrailSeconds;
                return true;
            }

            bool near = online && player->IsAlive() && player->GetMapId() == creature->GetMapId()
                && player->IsWithinDist(creature, config.leashYards, false);
            bool safety = near && (IsSafePlace(player) || GuardsNearby(player, player, config.guardYards)
                || GuardsNearby(creature, player, config.guardYards));
            if (near && !safety)
                return true;

            // After a death or a retreat it gives the player a moment; otherwise it just follows.
            if (safety || (online && !player->IsAlive()))
                hunt.waitSeconds = config.lostTrailSeconds;
            Disengage(hunt, creature);
            return true;
        }

        if (!online)
            return true;

        // A character can take up Hardcore after a group kill gave it a hunt.
        if (IsHardcore(hunt.guid))
        {
            DeleteHunt(hunt.guid);
            return false;
        }

        if (hunt.waitSeconds)
        {
            hunt.waitSeconds -= std::min(hunt.waitSeconds, tickSeconds);
            if (!hunt.waitSeconds)
            {
                hunt.warned = 0;
                Tell(player, hunt.map == player->GetMapId()
                    ? Acore::StringFormat("{} has picked up your trail, to the {}.", NameOf(hunt), DirectionFrom(player, hunt.x, hunt.y))
                    : Acore::StringFormat("{} has picked up your trail. It is a long way off, for now.", NameOf(hunt)));
                SaveHunt(hunt);
            }
            return true;
        }

        // A new progression phase raises the last rank's level, and the player may no longer be
        // close enough for it.
        if (hunt.rank >= FINAL_RANK && uint32(player->GetLevel()) + config.holdLevels < uint32(FinalLevelFor(player)))
        {
            hunt.rank = FINAL_RANK - 1;
            RollAffixes(hunt);
            SaveHunt(hunt);
        }

        bool inWorld = IsOpenWorld(player->GetMapId());
        if (inWorld)
        {
            hunt.lastKnown = true;
            hunt.lastMap = player->GetMapId();
            hunt.lastX = player->GetPositionX();
            hunt.lastY = player->GetPositionY();
        }
        else if (!hunt.lastKnown)
            return true;    // Logged in inside a dungeon. Nothing to head for yet.

        if (hunt.lastMap != hunt.map)
        {
            hunt.transitSeconds += tickSeconds;
            if (hunt.transitSeconds < config.continentSeconds)
                return true;

            float angle = frand(0.0f, 2.0f * float(M_PI));
            hunt.transitSeconds = 0;
            hunt.map = hunt.lastMap;
            hunt.x = hunt.lastX + std::cos(angle) * config.continentYards;
            hunt.y = hunt.lastY + std::sin(angle) * config.continentYards;
            hunt.warned = 0;
            SaveHunt(hunt);
            return true;
        }

        float distance = std::hypot(hunt.lastX - hunt.x, hunt.lastY - hunt.y);

        // The player got away, by hearthstone say: the warnings start over when it nears them again.
        if (hunt.warned && distance > config.farWarnYards)
            hunt.warned = 0;

        // The warnings in order, one a tick, and never on the tick it arrives.
        if (inWorld && hunt.warned < 1 && distance <= config.farWarnYards)
        {
            hunt.warned = 1;
            WhisperFrom(player, NameOf(hunt), Acore::StringFormat("I am coming for you, {}.", player->GetName()));
            Tell(player, Acore::StringFormat("{} is on its way, from the {}.", NameOf(hunt), DirectionFrom(player, hunt.x, hunt.y)));
            if (distance <= config.warnYards)
                return true;
        }

        if (inWorld && hunt.warned < 2 && distance <= config.warnYards)
        {
            hunt.warned = 2;
            std::string direction = DirectionFrom(player, hunt.x, hunt.y);
            if (WorldSession* session = player->GetSession())
                session->SendAreaTriggerMessage(Acore::StringFormat("Something is coming from the {}.", direction));
            Alarm(player);
            Tell(player, Acore::StringFormat("You feel watched. {} is close, to the {}.", NameOf(hunt), direction));
            StepTowards(hunt, hunt.lastX, hunt.lastY, distance, config.appearYards, float(tickSeconds));
            return true;
        }

        // A little over: rounding can leave it a hair short of the mark, and there it would stay.
        if (distance <= config.appearYards + 1.0f)
        {
            if (inWorld && CanAppear(player))
                Appear(hunt, player);
            return true;
        }

        StepTowards(hunt, hunt.lastX, hunt.lastY, distance, config.appearYards, float(tickSeconds));
        return true;
    }

    bool CanHaveNemesis(Player* player)
    {
        if (IsBot(player) || player->IsGameMaster())
            return false;

        return !IsHardcore(player->GetGUID().GetCounter());
    }

    void StartFrom(Creature* rare, Player* player, uint32 rareEntry, uint8 rank)
    {
        Hunt hunt;
        hunt.guid = player->GetGUID().GetCounter();
        hunt.rareEntry = rareEntry;
        hunt.rank = rank;
        hunt.waitSeconds = RandomWait();
        hunt.map = rare ? rare->GetMapId() : player->GetMapId();
        hunt.x = rare ? rare->GetPositionX() : player->GetPositionX();
        hunt.y = rare ? rare->GetPositionY() : player->GetPositionY();
        hunt.createdAt = uint32(GameTime::GetGameTime().count());
        RollAffixes(hunt);

        hunts[hunt.guid] = hunt;
        SaveHunt(hunt);
        RecordEvent(EVENT_STARTED, hunt, player->GetLevel(), false, 0);

        Tell(player, Acore::StringFormat("{} isn't finished with you. It will come back for you, wherever you go.", NameOf(hunt)));
    }

    // A rare died: does it swear revenge on anyone who killed it?
    void OnRareDeath(Creature* rare)
    {
        auto itr = copies.find(rare->GetEntry());
        if (itr == copies.end() || !IsOpenWorld(rare->GetMapId()))
            return;

        if (!rare->IsDamageEnoughForLootingAndReward() || rare->IsLootRewardDisabled())
            return;

        auto roll = [rare](Player* player)
        {
            if (!player || !CanHaveNemesis(player) || !player->IsAtGroupRewardDistance(rare))
                return;

            if (player->GetLevel() >= rare->GetLevel() + config.triggerMaxLevelsOver || !roll_chance_f(config.triggerChance))
                return;

            std::lock_guard<std::recursive_mutex> guard(huntsLock);
            if (!hunts.count(player->GetGUID().GetCounter()))
                StartFrom(rare, player, rare->GetEntry(), 1);
        };

        if (Group* group = rare->GetLootRecipientGroup())
        {
            for (GroupReference const* ref = group->GetFirstMember(); ref; ref = ref->next())
                roll(ref->GetSource());
            return;
        }

        roll(rare->GetLootRecipient());
    }

    uint32 MountItemFor(uint32 creatureType)
    {
        switch (creatureType)
        {
            case CREATURE_TYPE_HUMANOID:   return config.mountHumanoid;
            case CREATURE_TYPE_UNDEAD:     return config.mountUndead;
            case CREATURE_TYPE_BEAST:      return config.mountBeast;
            case CREATURE_TYPE_DEMON:      return config.mountDemon;
            case CREATURE_TYPE_ELEMENTAL:  return config.mountElemental;
            case CREATURE_TYPE_MECHANICAL: return config.mountMechanical;
            default:                       return config.mountOther;
        }
    }

    // The mount an item teaches: the spell its use-effect learns, or the spell a learn-trigger
    // row names. 0 for an item that teaches nothing.
    uint32 MountSpellOf(ItemTemplate const* proto)
    {
        for (_Spell const& spell : proto->Spells)
        {
            if (spell.SpellId <= 0)
                continue;

            if (spell.SpellTrigger == ITEM_SPELLTRIGGER_LEARN_SPELL_ID)
                return uint32(spell.SpellId);

            if (spell.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                continue;

            SpellInfo const* info = sSpellMgr->GetSpellInfo(uint32(spell.SpellId));
            if (!info)
                continue;

            for (SpellEffectInfo const& effect : info->GetEffects())
                if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && effect.TriggerSpell)
                    return effect.TriggerSpell;
        }

        return 0;
    }

    bool OwnsMount(Player* player, uint32 itemEntry, ItemTemplate const* proto)
    {
        if (player->HasItemCount(itemEntry, 1, true))
            return true;

        for (Mail const* mail : player->GetMails())
            for (MailItemInfo const& info : mail->items)
                if (info.item_template == itemEntry)
                    return true;

        uint32 spell = MountSpellOf(proto);
        return spell && player->HasSpell(spell);
    }

    void RollMount(Player* player, Creature* creature, Hunt const& hunt, bool botAssisted, uint32 seconds)
    {
        uint32 itemEntry = MountItemFor(creature->GetCreatureTemplate()->type);
        ItemTemplate const* proto = itemEntry ? sObjectMgr->GetItemTemplate(itemEntry) : nullptr;
        if (!proto)
            return;

        if (botAssisted)
        {
            Tell(player, "Bots fought at your side, so this victory carries no mount.");
            return;
        }

        if (OwnsMount(player, itemEntry, proto))
            return;

        // Each miss raises the account's next roll, so it can't go on failing for ever.
        uint32 account = player->GetSession()->GetAccountId();
        uint32& misses = mountMisses[account];
        if (!roll_chance_f(std::min(100.0f, config.mountChance + config.mountChanceStep * float(misses))))
        {
            ++misses;
            CharacterDatabase.Execute("REPLACE INTO mod_nemesis_luck (account, misses) VALUES ({}, {})", account, misses);
            Tell(player, "It leaves no trophy this time. The next one you finish will be more likely to.");
            return;
        }

        misses = 0;
        CharacterDatabase.Execute("DELETE FROM mod_nemesis_luck WHERE account = {}", account);
        RecordEvent(EVENT_MOUNT, hunt, player->GetLevel(), false, seconds);

        ItemPosCountVec dest;
        bool stored = false;
        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemEntry, 1) == EQUIP_ERR_OK)
            if (Item* item = player->StoreNewItem(dest, itemEntry, true))
            {
                player->SendNewItem(item, 1, true, false);
                stored = true;
            }

        // Full bags: by mail, as the core does for quest rewards.
        if (!stored)
            player->SendItemRetrievalMail(itemEntry, 1);

        Tell(player, Acore::StringFormat("Your nemesis leaves you a trophy: {}{}.", proto->Name1, stored ? "" : ", in your mailbox because your bags are full"));
    }

    void LoadHunts()
    {
        // A hunt of a character that is gone.
        CharacterDatabase.DirectExecute("DELETE h FROM mod_nemesis_hunt AS h LEFT JOIN characters AS c ON c.guid = h.guid WHERE c.guid IS NULL");

        std::lock_guard<std::recursive_mutex> guard(huntsLock);
        hunts.clear();

        mountMisses.clear();
        if (QueryResult result = CharacterDatabase.Query("SELECT account, misses FROM mod_nemesis_luck"))
        {
            do
            {
                Field* fields = result->Fetch();
                mountMisses[fields[0].Get<uint32>()] = fields[1].Get<uint32>();
            } while (result->NextRow());
        }

        if (QueryResult result = CharacterDatabase.Query("SELECT guid, rare_entry, `rank`, affixes, wait_seconds, map, x, y, created_at, kills, lesson, learned FROM mod_nemesis_hunt"))
        {
            do
            {
                Field* fields = result->Fetch();
                Hunt hunt;
                hunt.guid = fields[0].Get<uint32>();
                hunt.rareEntry = fields[1].Get<uint32>();
                hunt.rank = std::clamp<uint8>(fields[2].Get<uint8>(), 1, FINAL_RANK);
                hunt.affixes = fields[3].Get<uint32>();
                hunt.waitSeconds = fields[4].Get<uint32>();
                hunt.map = fields[5].Get<uint32>();
                hunt.x = fields[6].Get<float>();
                hunt.y = fields[7].Get<float>();
                hunt.createdAt = fields[8].Get<uint32>();
                hunt.kills = fields[9].Get<uint32>();
                hunt.lesson = fields[10].Get<uint8>();
                hunt.learned = fields[11].Get<uint32>();
                hunts[hunt.guid] = hunt;
            } while (result->NextRow());
        }
    }

    void LoadCopies()
    {
        copies.clear();

        if (QueryResult result = WorldDatabase.Query("SELECT rare_entry, hunter_entry, final_entry FROM mod_nemesis_creature"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 rare = fields[0].Get<uint32>();
                uint32 hunter = fields[1].Get<uint32>();
                uint32 final = fields[2].Get<uint32>();

                if (sObjectMgr->GetCreatureTemplate(rare) && sObjectMgr->GetCreatureTemplate(hunter) && sObjectMgr->GetCreatureTemplate(final))
                    copies[rare] = { hunter, final };
            } while (result->NextRow());
        }
    }
}

void Nemesis::LoadConfig()
{
    config.enabled = sConfigMgr->GetOption<bool>("Nemesis.Enable", true);
    config.triggerChance = sConfigMgr->GetOption<float>("Nemesis.TriggerChance", 5.0f);
    config.triggerMaxLevelsOver = sConfigMgr->GetOption<uint32>("Nemesis.TriggerMaxLevelsOver", 10);
    config.waitMinSeconds = sConfigMgr->GetOption<uint32>("Nemesis.WaitMinMinutes", 30) * MINUTE;
    config.waitMaxSeconds = sConfigMgr->GetOption<uint32>("Nemesis.WaitMaxMinutes", 60) * MINUTE;
    config.lostTrailSeconds = sConfigMgr->GetOption<uint32>("Nemesis.LostTrailSeconds", 60);
    config.tickSeconds = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Nemesis.TickSeconds", 5));
    config.speed = sConfigMgr->GetOption<float>("Nemesis.Speed", 16.0f);
    config.continentSeconds = sConfigMgr->GetOption<uint32>("Nemesis.ContinentSeconds", 300);
    config.continentYards = sConfigMgr->GetOption<float>("Nemesis.ContinentYards", 400.0f);
    config.farWarnYards = sConfigMgr->GetOption<float>("Nemesis.FarWarnYards", 800.0f);
    config.warnYards = sConfigMgr->GetOption<float>("Nemesis.WarnYards", 300.0f);
    config.farWarnYards = std::max(config.farWarnYards, config.warnYards);  // the warnings run far, then close
    config.warnSound = sConfigMgr->GetOption<uint32>("Nemesis.WarnSound", 8959);
    config.appearYards = sConfigMgr->GetOption<float>("Nemesis.AppearYards", 80.0f);
    config.spawnYards = sConfigMgr->GetOption<float>("Nemesis.SpawnYards", 35.0f);
    config.leashYards = sConfigMgr->GetOption<float>("Nemesis.LeashYards", 150.0f);
    config.guardYards = sConfigMgr->GetOption<float>("Nemesis.GuardYards", 60.0f);

    config.guardNames.clear();
    std::string guardNames = sConfigMgr->GetOption<std::string>("Nemesis.GuardNames", "Guard,Bruiser,Sentinel,Grunt,Peacekeeper,Deathguard,Defender,Warden");
    for (std::string_view word : Acore::Tokenize(guardNames, ',', false))
        config.guardNames.emplace_back(word);

    float const healthDefaults[] = { 1.5f, 2.0f, 2.5f, 3.0f };
    float const damageDefaults[] = { 1.25f, 1.5f, 2.0f, 2.5f };
    for (uint32 rank = 1; rank < FINAL_RANK; ++rank)
    {
        config.health[rank - 1] = sConfigMgr->GetOption<float>(Acore::StringFormat("Nemesis.Rank{}.Health", rank), healthDefaults[rank - 1]);
        config.damage[rank - 1] = sConfigMgr->GetOption<float>(Acore::StringFormat("Nemesis.Rank{}.Damage", rank), damageDefaults[rank - 1]);
    }

    config.finalHealth = { sConfigMgr->GetOption<float>("Nemesis.Final.Health60", 5.0f), sConfigMgr->GetOption<float>("Nemesis.Final.Health70", 4.0f), sConfigMgr->GetOption<float>("Nemesis.Final.Health80", 5.0f) };
    config.finalDamage = { sConfigMgr->GetOption<float>("Nemesis.Final.Damage60", 4.6f), sConfigMgr->GetOption<float>("Nemesis.Final.Damage70", 2.0f), sConfigMgr->GetOption<float>("Nemesis.Final.Damage80", 4.6f) };
    config.holdLevels = sConfigMgr->GetOption<uint32>("Nemesis.Final.HoldLevels", 5);

    float const scaleDefaults[] = { 1.2f, 1.3f, 1.4f, 1.5f, 1.6f };
    char const* const auraDefaults[] = { "63130", "63131", "63132", "63132,28126", "63132,28126,37816" };
    for (uint32 rank = 1; rank <= FINAL_RANK; ++rank)
    {
        config.scale[rank - 1] = sConfigMgr->GetOption<float>(Acore::StringFormat("Nemesis.Scale.Rank{}", rank), scaleDefaults[rank - 1]);

        std::vector<uint32>& auras = config.auras[rank - 1];
        auras.clear();
        std::string list = sConfigMgr->GetOption<std::string>(Acore::StringFormat("Nemesis.Aura.Rank{}", rank), auraDefaults[rank - 1]);
        for (std::string_view word : Acore::Tokenize(list, ',', false))
            if (Optional<uint32> spell = Acore::StringTo<uint32>(word))
                auras.push_back(*spell);    // Not checked: at startup the spells aren't loaded yet.
    }

    config.abilityBoltDamage = sConfigMgr->GetOption<float>("Nemesis.AbilityBoltDamage", 1.3f);
    config.abilityDotDamage = sConfigMgr->GetOption<float>("Nemesis.AbilityDotDamage", 0.2f);
    config.goldPerLevel = sConfigMgr->GetOption<uint32>("Nemesis.GoldPerLevel", 200);
    config.finalAchievement = sConfigMgr->GetOption<uint32>("Nemesis.Final.Achievement", 9510);
    config.mountChance = sConfigMgr->GetOption<float>("Nemesis.MountChance", 25.0f);
    config.mountChanceStep = sConfigMgr->GetOption<float>("Nemesis.MountChanceStep", 25.0f);
    config.championEnabled = sConfigMgr->GetOption<bool>("Nemesis.Champion.Enable", true);
    config.championTitle = sConfigMgr->GetOption<uint32>("Nemesis.Champion.Title", 192);
    config.mountHumanoid = sConfigMgr->GetOption<uint32>("Nemesis.Mount.Humanoid", 0);
    config.mountUndead = sConfigMgr->GetOption<uint32>("Nemesis.Mount.Undead", 0);
    config.mountBeast = sConfigMgr->GetOption<uint32>("Nemesis.Mount.Beast", 0);
    config.mountDemon = sConfigMgr->GetOption<uint32>("Nemesis.Mount.Demon", 0);
    config.mountElemental = sConfigMgr->GetOption<uint32>("Nemesis.Mount.Elemental", 0);
    config.mountMechanical = sConfigMgr->GetOption<uint32>("Nemesis.Mount.Mechanical", 0);
    config.mountOther = sConfigMgr->GetOption<uint32>("Nemesis.Mount.Other", 0);
}

void Nemesis::ReloadCopies()
{
    LoadCopies();
}

void Nemesis::LoadData()
{
    LoadCopies();
    LoadHunts();
    LOG_INFO("server.loading", ">> mod-nemesis: {} rares with copies, {} hunts under way", copies.size(), hunts.size());
}

bool Nemesis::IsBot(Player const* player)
{
    WorldSession const* session = player ? player->GetSession() : nullptr;
    return !session || session->IsHeadless();
}

uint8 Nemesis::FinalLevelFor(Player const* player)
{
    uint32 cap = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);

    if (sConfigMgr->GetOption<bool>("IndividualProgression.Enable", false, false))
    {
        uint32 state = 0;
        for (uint32 i = PROGRESSION_TBC; i <= PROGRESSION_LAST; ++i)
            if (player->GetQuestStatus(PROGRESSION_QUEST_BASE + i) == QUEST_STATUS_REWARDED)
                state = i;

        // A phase the realm hasn't opened counts as not reached, as the module itself has it.
        if (uint32 limit = sConfigMgr->GetOption<uint32>("IndividualProgression.ProgressionLimit", 0, false))
            state = std::min(state, limit);

        cap = state >= PROGRESSION_WOTLK ? 80 : state >= PROGRESSION_TBC ? 70 : 60;
    }

    return uint8(std::max<uint32>(cap, player->GetLevel()));
}

void Nemesis::OnNemesisDied(Creature* creature, ObjectGuid owner, FightReport const& report)
{
    std::lock_guard<std::recursive_mutex> guard(huntsLock);

    auto itr = hunts.find(owner.GetCounter());
    if (itr == hunts.end() || itr->second.creature != creature->GetGUID())
        return;

    Hunt& hunt = itr->second;
    hunt.creature.Clear();
    hunt.map = creature->GetMapId();
    hunt.x = creature->GetPositionX();
    hunt.y = creature->GetPositionY();
    hunt.warned = 0;
    hunt.waitSeconds = RandomWait();

    // Only the owner's own kill, or their group's, counts. The owner has to be on this map: this
    // runs on its thread.
    Player* player = ObjectAccessor::GetPlayer(*creature, owner);
    Group* group = player ? player->GetGroup() : nullptr;
    bool credited = player && creature->IsDamageEnoughForLootingAndReward()
        && (creature->GetLootRecipient() == player || (group && creature->GetLootRecipientGroup() == group));

    if (!credited)
    {
        if (player)
            Tell(player, Acore::StringFormat("Someone else cut {} down. It will be back for you.", NameOf(hunt)));
        RecordEvent(EVENT_STOLEN, hunt, player ? player->GetLevel() : 0, report.botAssisted, report.seconds);
        SaveHunt(hunt);
        return;
    }

    bool botAssisted = report.botAssisted;
    bool final = hunt.rank >= FINAL_RANK;
    ++hunt.kills;
    uint32 gold = config.goldPerLevel * uint32(player->GetLevel()) * uint32(hunt.rank) * (final ? 2 : 1);
    if (gold)
        player->ModifyMoney(int32(gold));

    if (final)
    {
        Tell(player, Acore::StringFormat("{} is dead, for good. The hunt is over, after {} kills.", NameOf(hunt), hunt.kills));
        RecordEvent(EVENT_FINISHED, hunt, player->GetLevel(), botAssisted, report.seconds);

        if (AchievementEntry const* achievement = config.finalAchievement ? sAchievementStore.LookupEntry(config.finalAchievement) : nullptr)
            player->CompletedAchievement(achievement);

        RollMount(player, creature, hunt, botAssisted, report.seconds);
        DeleteHunt(hunt.guid);
        hunts.erase(itr);
        return;
    }

    RecordEvent(EVENT_KILLED, hunt, player->GetLevel(), botAssisted, report.seconds);

    if (report.lesson != LESSON_NONE)
    {
        hunt.lesson = report.lesson;
        hunt.learned |= AffixOf(report.lesson);
    }

    std::string name = NameOf(hunt);
    bool hold = hunt.rank + 1 == FINAL_RANK && uint32(player->GetLevel()) + config.holdLevels < uint32(FinalLevelFor(player));
    if (hold)
        Tell(player, Acore::StringFormat("{} falls again, still rank {} {}. It won't show its true strength until you are stronger.", name, uint32(hunt.rank), RankName(hunt.rank)));
    else
    {
        ++hunt.rank;
        Tell(player, Acore::StringFormat("{} falls, but it isn't finished. It will return at rank {} {}.", name, uint32(hunt.rank), RankName(hunt.rank)));
    }

    RollAffixes(hunt);
    if (report.lesson != LESSON_NONE)
    {
        Tell(player, Acore::StringFormat("It will remember how you fought: it comes back {}.", AffixNames(AffixOf(report.lesson))));
        RecordEvent(EVENT_LEARNED, hunt, player->GetLevel(), botAssisted, report.seconds);
    }

    SaveHunt(hunt);
}

void Nemesis::OnNemesisWon(Creature* creature, Player* owner, bool botAssisted, uint32 seconds)
{
    std::lock_guard<std::recursive_mutex> guard(huntsLock);

    auto itr = hunts.find(owner->GetGUID().GetCounter());
    if (itr == hunts.end() || itr->second.creature != creature->GetGUID())
        return;

    Tell(owner, Acore::StringFormat("{} has had its revenge and hunts you no more. Your streak ends at rank {}, after {} kills.",
        NameOf(itr->second), uint32(itr->second.rank), itr->second.kills));
    RecordEvent(EVENT_WON, itr->second, owner->GetLevel(), botAssisted, seconds);
    DeleteHunt(itr->first);
    hunts.erase(itr);
}

void Nemesis::OnNemesisLostTrail(Creature* creature, ObjectGuid owner)
{
    std::lock_guard<std::recursive_mutex> guard(huntsLock);

    auto itr = hunts.find(owner.GetCounter());
    if (itr == hunts.end() || itr->second.creature != creature->GetGUID())
        return;

    Hunt& hunt = itr->second;
    hunt.creature.Clear();
    hunt.map = creature->GetMapId();
    hunt.x = creature->GetPositionX();
    hunt.y = creature->GetPositionY();
    hunt.warned = 0;
    hunt.waitSeconds = config.lostTrailSeconds;
    SaveHunt(hunt);
}

Nemesis::Status Nemesis::StatusOf(Player const* player)
{
    std::lock_guard<std::recursive_mutex> guard(huntsLock);

    Status status;
    auto itr = hunts.find(player->GetGUID().GetCounter());
    if (itr == hunts.end())
        return status;

    Hunt const& hunt = itr->second;
    status.found = true;
    status.rareEntry = hunt.rareEntry;
    status.rank = hunt.rank;
    status.affixes = hunt.affixes;
    status.waitSeconds = hunt.waitSeconds;
    status.engaged = !hunt.creature.IsEmpty();
    status.kills = hunt.kills;
    status.name = NameOf(hunt);
    if (!status.engaged && !hunt.waitSeconds && hunt.map == player->GetMapId())
        status.direction = DirectionFrom(player, hunt.x, hunt.y);
    return status;
}

std::string Nemesis::RankName(uint8 rank)
{
    switch (rank)
    {
        case 1:  return "Marked";
        case 2:  return "Hated";
        case 3:  return "Relentless";
        case 4:  return "Legendary";
        default: return "Mythic";
    }
}

std::string Nemesis::AffixNames(uint32 affixes)
{
    static std::pair<uint32, char const*> const names[] =
    {
        { AFFIX_VAMPIRIC, "Vampiric" }, { AFFIX_SWIFT, "Swift" }, { AFFIX_JUGGERNAUT, "Juggernaut" }, { AFFIX_SAVAGE, "Savage" },
        { AFFIX_SPELLWARD, "Spellward" }, { AFFIX_ENRAGED, "Enraged" }, { AFFIX_REGENERATING, "Regenerating" },
    };

    std::string result;
    for (auto const& [bit, name] : names)
        if (affixes & bit)
        {
            if (!result.empty())
                result += ", ";
            result += name;
        }
    return result;
}

bool Nemesis::StartHunt(Player* player, uint32 rareEntry, uint8 rank)
{
    std::lock_guard<std::recursive_mutex> guard(huntsLock);

    if (hunts.count(player->GetGUID().GetCounter()) || !copies.count(rareEntry))
        return false;

    StartFrom(nullptr, player, rareEntry, std::clamp<uint8>(rank, 1, FINAL_RANK));
    return true;
}

bool Nemesis::SetRank(Player* player, uint8 rank)
{
    std::lock_guard<std::recursive_mutex> guard(huntsLock);

    auto itr = hunts.find(player->GetGUID().GetCounter());
    if (itr == hunts.end() || !itr->second.creature.IsEmpty())
        return false;

    itr->second.rank = std::clamp<uint8>(rank, 1, FINAL_RANK);
    RollAffixes(itr->second);
    SaveHunt(itr->second);
    return true;
}

bool Nemesis::HuntNow(Player* player)
{
    std::lock_guard<std::recursive_mutex> guard(huntsLock);

    auto itr = hunts.find(player->GetGUID().GetCounter());
    if (itr == hunts.end() || !itr->second.creature.IsEmpty() || !IsOpenWorld(player->GetMapId()))
        return false;

    Hunt& hunt = itr->second;
    hunt.waitSeconds = 0;
    hunt.transitSeconds = 0;
    hunt.warned = 0;
    hunt.map = player->GetMapId();
    hunt.x = player->GetPositionX() + config.warnYards + 100.0f;   // so all the warnings play out
    hunt.y = player->GetPositionY();
    return true;
}

bool Nemesis::ClearHunt(Player* player)
{
    std::lock_guard<std::recursive_mutex> guard(huntsLock);

    auto itr = hunts.find(player->GetGUID().GetCounter());
    if (itr == hunts.end())
        return false;

    if (!itr->second.creature.IsEmpty())
        if (Map* map = sMapMgr->FindBaseMap(itr->second.map))
            if (Creature* creature = map->GetCreature(itr->second.creature))
                creature->DespawnOrUnsummon();

    DeleteHunt(itr->first);
    hunts.erase(itr);
    return true;
}

class NemesisWorldScript : public WorldScript
{
public:
    NemesisWorldScript() : WorldScript("NemesisWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
    }

    void OnStartup() override
    {
        LoadData();
    }

    void OnUpdate(uint32 diff) override
    {
        if (!config.enabled)
            return;

        tickTimer += diff;
        if (tickTimer < config.tickSeconds * IN_MILLISECONDS)
            return;
        tickTimer = 0;

        std::lock_guard<std::recursive_mutex> guard(huntsLock);
        for (auto itr = hunts.begin(); itr != hunts.end();)
        {
            if (StepHunt(itr->second, config.tickSeconds))
                ++itr;
            else
                itr = hunts.erase(itr);
        }
    }
};

class NemesisUnitScript : public UnitScript
{
public:
    NemesisUnitScript() : UnitScript("NemesisUnitScript", true, { UNITHOOK_ON_UNIT_DEATH }) { }

    // Every death in the world comes through here, so the first question is whether it was a
    // creature with copies: one lookup in a table that doesn't change.
    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        if (config.enabled && unit->IsCreature())
            OnRareDeath(unit->ToCreature());
    }
};

class NemesisPlayerScript : public PlayerScript
{
public:
    NemesisPlayerScript() : PlayerScript("NemesisPlayerScript", { PLAYERHOOK_ON_LOGOUT }) { }

    // A hunt's position only reaches the database when something happens to it, so a player
    // leaving is the moment to keep where it got to.
    void OnPlayerLogout(Player* player) override
    {
        std::lock_guard<std::recursive_mutex> guard(huntsLock);

        auto itr = hunts.find(player->GetGUID().GetCounter());
        if (itr != hunts.end())
            SaveHunt(itr->second);
    }
};

void AddNemesisScripts()
{
    new NemesisWorldScript();
    new NemesisUnitScript();
    new NemesisPlayerScript();
}
