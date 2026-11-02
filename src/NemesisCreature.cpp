/*
 * mod-nemesis: the creature itself, from the moment it appears in front of its target.
 *
 * It's a temporary summon with its own object, so nothing it gains here outlasts it: a new fight
 * starts from a new creature. Stats come from the level it fights at, not from the rare's.
 *
 * Abilities come from its creature type, or a caster's set if the rare was a mage, and open up with
 * rank. A rare's own spells are made for the rare's level, so these are stock NPC spells with their
 * damage worked out from the creature's melee hit: they stay a threat at any level.
 *
 * While it fights it watches how it's being beaten, and hands the lesson to the hunt when it dies.
 *
 * Released under the MIT License.
 */

#include "Nemesis.h"
#include "CreatureAI.h"
#include "CreatureScript.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"

#include <algorithm>
#include <vector>

using namespace Nemesis;

namespace
{
    constexpr float VAMPIRIC_HEAL = 0.25f;
    constexpr float SWIFT_SPEED = 1.3f;
    constexpr float SWIFT_ATTACK_TIME = 0.75f;
    constexpr float SAVAGE_DAMAGE = 1.25f;
    constexpr float SPELLWARD_DAMAGE = 0.7f;
    constexpr float ENRAGED_HEALTH_PCT = 30.0f;
    constexpr float ENRAGED_DAMAGE = 1.5f;
    constexpr float REGENERATING_PCT = 2.0f;
    constexpr uint32 REGENERATING_SECONDS = 5;
    constexpr uint32 NOT_ENGAGED_SECONDS = 3;
    constexpr uint32 BASE_ATTACK_MS = 2000;

    constexpr uint32 FIRST_ABILITY_MS = 6000;
    constexpr uint32 BETWEEN_ABILITIES_MS = 5000;
    constexpr uint32 FAILED_ABILITY_MS = 2000;
    constexpr float CHARGE_MIN_YARDS = 8.0f;
    constexpr float CHARGE_MAX_YARDS = 25.0f;
    constexpr float CHARGE_DAMAGE = 0.25f;          // of its melee hit, on top of the weapon damage
    constexpr float SHORT_RANGE_YARDS = 10.0f;      // for what it does to everyone around it

    // What it takes to learn from a fight.
    constexpr uint32 LESSON_CONTROL_HITS = 2;
    constexpr uint32 LESSON_APART_SECONDS = 10;
    constexpr float LESSON_APART_SHARE = 0.4f;
    constexpr float LESSON_SPELL_SHARE = 0.6f;
    constexpr float LESSON_OUTLASTED_HEALTHS = 1.5f;

    // Not snares: a Frostbolt's slow says more about the spell than about how the fight went.
    uint64 const CONTROL_MECHANICS = (1ULL << MECHANIC_CHARM) | (1ULL << MECHANIC_DISORIENTED) | (1ULL << MECHANIC_FEAR)
        | (1ULL << MECHANIC_ROOT) | (1ULL << MECHANIC_SLEEP) | (1ULL << MECHANIC_STUN)
        | (1ULL << MECHANIC_FREEZE) | (1ULL << MECHANIC_KNOCKOUT) | (1ULL << MECHANIC_POLYMORPH)
        | (1ULL << MECHANIC_BANISH) | (1ULL << MECHANIC_HORROR) | (1ULL << MECHANIC_SAPPED);

    uint32 const JUGGERNAUT_MECHANICS[] =
    {
        MECHANIC_DISORIENTED, MECHANIC_FEAR, MECHANIC_ROOT, MECHANIC_SLEEP, MECHANIC_SNARE, MECHANIC_STUN,
        MECHANIC_FREEZE, MECHANIC_KNOCKOUT, MECHANIC_POLYMORPH, MECHANIC_BANISH, MECHANIC_HORROR, MECHANIC_SAPPED,
    };

    enum AbilityKind : uint8
    {
        ABILITY_BOLT,       // a spell whose damage is ours to set
        ABILITY_DOT,        // the same, per tick
        ABILITY_CONTROL,    // no damage of its own: fear, slow, weaken
        ABILITY_NEARBY,     // hits around itself
        ABILITY_CHARGE,     // closes the gap
    };

    struct Ability
    {
        uint32 spell;
        AbilityKind kind;
        uint32 cooldownMs;
        uint8 fromRank;
    };

    // The three spells of a kit open at ranks 1, 3 and 5.
    std::vector<Ability> const KIT_FIGHTER = { { 16509, ABILITY_DOT, 18000, 1 }, { 22120, ABILITY_CHARGE, 15000, 3 }, { 9080, ABILITY_CONTROL, 20000, 5 } };
    std::vector<Ability> const KIT_CASTER = { { 9053, ABILITY_BOLT, 8000, 1 }, { 12542, ABILITY_CONTROL, 30000, 3 }, { 12553, ABILITY_BOLT, 12000, 5 } };
    std::vector<Ability> const KIT_UNDEAD = { { 9613, ABILITY_BOLT, 8000, 1 }, { 12542, ABILITY_CONTROL, 30000, 3 }, { 11639, ABILITY_DOT, 18000, 5 } };
    std::vector<Ability> const KIT_BEAST = { { 13443, ABILITY_DOT, 18000, 1 }, { 22120, ABILITY_CHARGE, 15000, 3 }, { 14100, ABILITY_NEARBY, 30000, 5 } };
    std::vector<Ability> const KIT_DEMON = { { 9613, ABILITY_BOLT, 8000, 1 }, { 11980, ABILITY_CONTROL, 25000, 3 }, { 12542, ABILITY_CONTROL, 30000, 5 } };
    std::vector<Ability> const KIT_ELEMENTAL = { { 9532, ABILITY_BOLT, 8000, 1 }, { 12553, ABILITY_BOLT, 12000, 3 }, { 6524, ABILITY_NEARBY, 30000, 5 } };
    std::vector<Ability> const KIT_MECHANICAL = { { 6660, ABILITY_BOLT, 6000, 1 }, { 22120, ABILITY_CHARGE, 15000, 3 }, { 6524, ABILITY_NEARBY, 30000, 5 } };
    std::vector<Ability> const KIT_DRAGONKIN = { { 9053, ABILITY_BOLT, 8000, 1 }, { 22120, ABILITY_CHARGE, 15000, 3 }, { 14100, ABILITY_NEARBY, 30000, 5 } };
    std::vector<Ability> const KIT_GIANT = { { 16509, ABILITY_DOT, 18000, 1 }, { 22120, ABILITY_CHARGE, 15000, 3 }, { 6524, ABILITY_NEARBY, 30000, 5 } };

    std::vector<Ability> const& KitFor(Creature const* creature, bool caster)
    {
        switch (creature->GetCreatureTemplate()->type)
        {
            case CREATURE_TYPE_UNDEAD:     return KIT_UNDEAD;
            case CREATURE_TYPE_BEAST:      return KIT_BEAST;
            case CREATURE_TYPE_DEMON:      return KIT_DEMON;
            case CREATURE_TYPE_ELEMENTAL:  return KIT_ELEMENTAL;
            case CREATURE_TYPE_MECHANICAL: return KIT_MECHANICAL;
            case CREATURE_TYPE_DRAGONKIN:  return KIT_DRAGONKIN;
            case CREATURE_TYPE_GIANT:      return KIT_GIANT;
            default:                       return caster ? KIT_CASTER : KIT_FIGHTER;
        }
    }

    char const* const TALK_ARRIVE[FINAL_RANK] =
    {
        "You thought that was the end of it, {}?",
        "I remember your face, {}.",
        "Every time you cut me down I come back stronger, {}!",
        "No matter where you run, {}, I will find you.",
        "I have died by your hand for the last time, {}!",
    };
    char const* const TALK_WIN = "Now we are even, {}.";
    char const* const TALK_DIE = "This... is not over... {}.";
    char const* const TALK_DIE_FINAL = "At last... you have... ended it... {}.";

    // What it says when it comes back having learned something, by Lesson.
    char const* const TALK_LESSON[LESSON_COUNT] =
    {
        "",
        "Your magic won't save you this time, {}.",
        "Your tricks won't hold me again, {}.",
        "You won't run from me this time, {}.",
        "Heal all you like, {}. I hit harder now.",
    };

    // %s is the creature's name, which the client fills in.
    char const* const EMOTE_ARRIVE = "%s has tracked you down, {}!";
    char const* const EMOTE_WIN = "%s stands over {}. Its hunt is over.";
    char const* const EMOTE_DIE = "%s falls, its eyes still fixed on {}.";
    char const* const EMOTE_DIE_FINAL = "%s falls, and will not rise again.";
    char const* const EMOTE_LESSON[LESSON_COUNT] =
    {
        "",
        "%s shrugs off the memory of your magic, {}.",
        "%s has grown too wild to hold, {}.",
        "%s comes at you faster than before, {}.",
        "%s bears down on you harder than before, {}.",
    };

    float HealthOf(Fight const& fight)
    {
        if (fight.rank < FINAL_RANK)
            return config.health[fight.rank - 1];
        return config.finalHealth[fight.level <= 60 ? 0 : fight.level <= 70 ? 1 : 2];
    }

    float DamageOf(Fight const& fight)
    {
        if (fight.rank < FINAL_RANK)
            return config.damage[fight.rank - 1];
        return config.finalDamage[fight.level <= 60 ? 0 : fight.level <= 70 ? 1 : 2];
    }
}

struct npc_mod_nemesis : public ScriptedAI
{
    explicit npc_mod_nemesis(Creature* creature) : ScriptedAI(creature) { }

    void Reset() override { }

    // It came for one player; it doesn't go after everyone it walks past.
    void MoveInLineOfSight(Unit* /*who*/) override { }

    void Begin(Fight const& fight)
    {
        _fight = fight;
        _ready = true;

        for (Ability const& ability : KitFor(me, fight.caster))
            if (ability.fromRank <= fight.rank && sSpellMgr->GetSpellInfo(ability.spell))
                _abilities.push_back({ ability, 0 });
        _abilityTimer = FIRST_ABILITY_MS;

        ApplyAffixes();
        ApplyStats();
        KeepAuras();

        // A charmed creature runs another AI, so none of the outcomes here would happen.
        me->ApplySpellImmune(0, IMMUNITY_MECHANIC, MECHANIC_CHARM, true);

        if (Player* owner = ObjectAccessor::GetPlayer(*me, fight.owner))
        {
            if (fight.lesson != LESSON_NONE && fight.lesson < LESSON_COUNT)
                Say(TALK_LESSON[fight.lesson], EMOTE_LESSON[fight.lesson], owner);
            else
                Say(TALK_ARRIVE[fight.rank - 1], EMOTE_ARRIVE, owner);

            AttackStart(owner);
            me->SetAssistanceTimer(0);
        }
    }

    void DamageTaken(Unit* attacker, uint32& damage, DamageEffectType /*type*/, SpellSchoolMask school) override
    {
        if (attacker)
            if (Player* player = attacker->GetAffectingPlayer(); player && IsBot(player))
                _botAssisted = true;

        bool spell = !(school & SPELL_SCHOOL_MASK_NORMAL);
        if (spell && (_fight.affixes & AFFIX_SPELLWARD))
            damage = uint32(float(damage) * SPELLWARD_DAMAGE);

        (spell ? _spellDamageTaken : _meleeDamageTaken) += damage;
    }

    void SpellHit(Unit* /*caster*/, SpellInfo const* spell) override
    {
        if (spell && (spell->GetAllEffectsMechanicMask() & CONTROL_MECHANICS))
            ++_controlHits;
    }

    void DamageDealt(Unit* victim, uint32& damage, DamageEffectType /*type*/, SpellSchoolMask /*school*/) override
    {
        // Its rank's damage is in its stats (ApplyStats), so armor, block and shields count as
        // usual. Only this part changes during a fight, so it can't be.
        if ((_fight.affixes & AFFIX_ENRAGED) && me->HealthBelowPct(int32(ENRAGED_HEALTH_PCT)))
            damage = uint32(float(damage) * ENRAGED_DAMAGE);

        if (victim && victim->GetGUID() == _fight.owner)
            _dealtToOwner += damage;

        if ((_fight.affixes & AFFIX_VAMPIRIC) && damage && me->IsAlive())
            me->ModifyHealth(int32(float(damage) * VAMPIRIC_HEAL));
    }

    void KilledUnit(Unit* victim) override
    {
        // Dead already, and something it left behind finished the job: the corpse stays lootable.
        if (!me->IsAlive() || victim->GetGUID() != _fight.owner)
            return;

        Player* owner = victim->ToPlayer();
        if (!owner)
            return;

        Say(TALK_WIN, EMOTE_WIN, owner);
        OnNemesisWon(me, owner, _botAssisted, _fightSeconds);
        me->DespawnOrUnsummon(3s);
    }

    void JustDied(Unit* /*killer*/) override
    {
        Player* owner = ObjectAccessor::GetPlayer(*me, _fight.owner);
        if (owner)
        {
            if (_fight.rank >= FINAL_RANK)
                Say(TALK_DIE_FINAL, EMOTE_DIE_FINAL, owner);
            else
                Say(TALK_DIE, EMOTE_DIE, owner);
        }

        FightReport report;
        report.botAssisted = _botAssisted;
        report.seconds = _fightSeconds;
        report.lesson = LessonFrom(owner);
        OnNemesisDied(me, _fight.owner, report);
    }

    void EnterEvadeMode(EvadeReason /*why*/) override
    {
        // There's no home to walk back to: it lost its target and goes back to following.
        OnNemesisLostTrail(me, _fight.owner);
        me->DespawnOrUnsummon();
    }

    void UpdateAI(uint32 diff) override
    {
        if (!_ready)
            return;

        _secondTimer += diff;
        if (_secondTimer >= 1000)
        {
            _secondTimer -= 1000;
            OnSecond();
            if (!me->IsAlive() || !me->IsInWorld())
                return;
        }

        if (!UpdateVictim())
            return;

        for (Slot& slot : _abilities)
            slot.readyInMs -= std::min(slot.readyInMs, diff);
        _abilityTimer -= std::min(_abilityTimer, diff);

        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        if (!_abilityTimer)
            UseAbility();

        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        DoMeleeAttackIfReady();
    }

private:
    struct Slot
    {
        Ability ability;
        uint32 readyInMs;
    };

    void Say(char const* talk, char const* emote, Player* owner)
    {
        if (IsTalker(me))
            me->Yell(Acore::StringFormat(talk, owner->GetName()), LANG_UNIVERSAL);
        else
            me->TextEmote(Acore::StringFormat(emote, owner->GetName()));
    }

    // The first lesson of this fight that it hasn't learned yet.
    Lesson LessonFrom(Player const* owner) const
    {
        uint32 spell = _spellDamageTaken;
        uint32 total = spell + _meleeDamageTaken;

        std::pair<Lesson, bool> const seen[] =
        {
            { LESSON_CONTROL, _controlHits >= LESSON_CONTROL_HITS },
            { LESSON_KITED, _apartSeconds >= LESSON_APART_SECONDS && float(_apartSeconds) >= float(_fightSeconds) * LESSON_APART_SHARE },
            { LESSON_MAGIC, total && float(spell) >= float(total) * LESSON_SPELL_SHARE },
            { LESSON_OUTLASTED, owner && float(_dealtToOwner) >= float(owner->GetMaxHealth()) * LESSON_OUTLASTED_HEALTHS },
        };

        for (auto const& [lesson, happened] : seen)
            if (happened && !(_fight.affixes & AffixOf(lesson)))
                return lesson;

        return LESSON_NONE;
    }

    void UseAbility()
    {
        Unit* victim = me->GetVictim();
        if (!victim)
            return;

        // Its melee hit, as the stats have it: what its spells are measured against.
        int32 hit = int32((me->GetFloatValue(UNIT_FIELD_MINDAMAGE) + me->GetFloatValue(UNIT_FIELD_MAXDAMAGE)) / 2.0f);
        float distance = me->GetDistance(victim);

        for (Slot& slot : _abilities)
        {
            if (slot.readyInMs)
                continue;

            Ability const& ability = slot.ability;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(ability.spell);
            if (!info || me->IsSpellProhibited(info->GetSchoolMask()))
                continue;

            SpellCastResult result = SPELL_CAST_OK;
            switch (ability.kind)
            {
                case ABILITY_CHARGE:
                    if (distance < CHARGE_MIN_YARDS || distance > CHARGE_MAX_YARDS)
                        continue;
                    result = me->CastCustomSpell(ability.spell, SPELLVALUE_BASE_POINT1, int32(float(hit) * CHARGE_DAMAGE), victim, TRIGGERED_IGNORE_POWER_AND_REAGENT_COST);
                    break;
                case ABILITY_NEARBY:
                    if (distance > SHORT_RANGE_YARDS)
                        continue;
                    result = me->CastSpell(me, ability.spell, TRIGGERED_IGNORE_POWER_AND_REAGENT_COST);
                    break;
                case ABILITY_BOLT:
                    if (distance > info->GetMaxRange(false))
                        continue;
                    result = me->CastCustomSpell(ability.spell, SPELLVALUE_BASE_POINT0, UnscaledDamage(info, float(hit) * config.abilityBoltDamage), victim, TRIGGERED_IGNORE_POWER_AND_REAGENT_COST);
                    break;
                case ABILITY_DOT:
                    if (distance > info->GetMaxRange(false))
                        continue;
                    result = me->CastCustomSpell(ability.spell, SPELLVALUE_BASE_POINT0, UnscaledDamage(info, float(hit) * config.abilityDotDamage), victim, TRIGGERED_IGNORE_POWER_AND_REAGENT_COST);
                    break;
                case ABILITY_CONTROL:
                    if (distance > info->GetMaxRange(false))
                        continue;
                    result = me->CastSpell(victim, ability.spell, TRIGGERED_IGNORE_POWER_AND_REAGENT_COST);
                    break;
            }

            if (result == SPELL_CAST_OK)
            {
                slot.readyInMs = ability.cooldownMs;
                _abilityTimer = BETWEEN_ABILITIES_MS;
                return;
            }

            // Too close, say: the next one may still work.
            slot.readyInMs = FAILED_ABILITY_MS;
        }

        _abilityTimer = FAILED_ABILITY_MS;
    }

    // Stock NPC spells scale with the caster's level on top of any value they're given
    // (SpellEffectInfo::CalcValue), so the value goes in with that taken out again.
    int32 UnscaledDamage(SpellInfo const* info, float wanted) const
    {
        if (info->SpellLevel && info->SpellLevel != me->GetLevel() && info->HasAttribute(SPELL_ATTR0_SCALES_WITH_CREATURE_LEVEL))
        {
            uint8 expansion = me->GetCreatureTemplate()->expansion;
            float atLevel = sObjectMgr->GetCreatureBaseStats(me->GetLevel(), me->getClass())->BaseDamage[expansion];
            float atSpellLevel = sObjectMgr->GetCreatureBaseStats(info->SpellLevel, me->getClass())->BaseDamage[expansion];
            if (atLevel > 0.0f)
                wanted *= atSpellLevel / atLevel;
        }

        return std::max<int32>(1, int32(wanted));
    }

    void ApplyStats()
    {
        me->SetLevel(_fight.level);

        CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(_fight.level, CLASS_WARRIOR);

        // A level's stats per expansion: from 58 and 68 up the later columns are filled in.
        uint8 expansion = _fight.level > 70 ? 2 : _fight.level > 60 ? 1 : 0;
        while (expansion > 0 && stats->BaseHealth[expansion] <= 1)
            --expansion;

        uint32 health = std::max<uint32>(1, uint32(float(stats->BaseHealth[expansion]) * HealthOf(_fight)));

        // Both halves of a melee hit, as a template's damage modifier would.
        float damage = DamageOf(_fight) * ((_fight.affixes & AFFIX_SAVAGE) ? SAVAGE_DAMAGE : 1.0f);

        // The core makes a faster attack hit for less. Swift is meant to be a gain.
        if (_fight.affixes & AFFIX_SWIFT)
            damage /= SWIFT_ATTACK_TIME;

        float weapon = stats->BaseDamage[expansion] * damage;

        me->SetCreateHealth(health);
        me->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, float(health));
        me->SetStatFlatModifier(UNIT_MOD_ARMOR, BASE_VALUE, stats->BaseArmor);
        me->SetStatFlatModifier(UNIT_MOD_ATTACK_POWER, BASE_VALUE, float(stats->AttackPower) * damage);
        me->SetStatFlatModifier(UNIT_MOD_ATTACK_POWER_RANGED, BASE_VALUE, float(stats->RangedAttackPower) * damage);

        for (WeaponAttackType attack : { BASE_ATTACK, OFF_ATTACK, RANGED_ATTACK })
        {
            me->SetBaseWeaponDamage(attack, MINDAMAGE, weapon);
            me->SetBaseWeaponDamage(attack, MAXDAMAGE, weapon * 1.5f);
        }

        me->SetObjectScale(me->GetNativeObjectScale() * config.scale[_fight.rank - 1]);
        me->UpdateAllStats();
        me->SetFullHealth();

        // SelectLevel sized this for the template's health, before the numbers above.
        me->ResetPlayerDamageReq();
    }

    void KeepAuras()
    {
        for (uint32 spell : config.auras[_fight.rank - 1])
            if (!me->HasAura(spell))
                me->AddAura(spell, me);
    }

    void ApplyAffixes()
    {
        if (_fight.affixes & AFFIX_SWIFT)
        {
            _swiftRate = me->GetSpeedRate(MOVE_RUN) * SWIFT_SPEED;
            me->SetSpeed(MOVE_RUN, _swiftRate, true);
            me->SetAttackTime(BASE_ATTACK, uint32(BASE_ATTACK_MS * SWIFT_ATTACK_TIME));
            me->SetAttackTime(OFF_ATTACK, uint32(BASE_ATTACK_MS * SWIFT_ATTACK_TIME));
        }

        if (_fight.affixes & AFFIX_JUGGERNAUT)
        {
            for (uint32 mechanic : JUGGERNAUT_MECHANICS)
                me->ApplySpellImmune(0, IMMUNITY_MECHANIC, mechanic, true);
            me->ApplySpellImmune(0, IMMUNITY_EFFECT, SPELL_EFFECT_KNOCK_BACK, true);
            me->ApplySpellImmune(0, IMMUNITY_EFFECT, SPELL_EFFECT_KNOCK_BACK_DEST, true);
        }
    }

    void OnSecond()
    {
        // The core's leash runs from the point it appeared at, and that's already most of the
        // leash away from the player. How far a fight may go is the hunt's call (LeashYards).
        me->UpdateLeashExtensionTime();

        // A fight with it shouldn't pull every monster around it. Each new target arms this again.
        me->SetAssistanceTimer(0);

        KeepAuras();

        // The core works speed out again from the template whenever a slow comes or goes.
        if (_swiftRate > 0.0f && me->GetSpeedRate(MOVE_RUN) < _swiftRate && !me->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED))
            me->SetSpeed(MOVE_RUN, _swiftRate, true);

        // Bots healing or buffing never touch the creature, so being in a group with one at any
        // moment of the fight counts. Kicking them before the last hit changes nothing.
        if (!_botAssisted)
            if (Player* owner = ObjectAccessor::GetPlayer(*me, _fight.owner))
                if (Group const* group = owner->GetGroup())
                    for (GroupReference const* ref = group->GetFirstMember(); ref; ref = ref->next())
                        if (Player const* member = ref->GetSource(); member && IsBot(member))
                            _botAssisted = true;

        if ((_fight.affixes & AFFIX_REGENERATING) && me->IsAlive() && me->GetHealth() < me->GetMaxHealth()
            && ++_regenSeconds >= REGENERATING_SECONDS)
        {
            _regenSeconds = 0;
            me->ModifyHealth(int32(me->CountPctFromMaxHealth(int32(REGENERATING_PCT))));
        }

        // Nobody it can fight: vanish, feign death, or a target it could never start on.
        if (me->IsEngaged())
        {
            _notEngagedSeconds = 0;
            ++_fightSeconds;
            if (Unit const* victim = me->GetVictim(); victim && !me->HasUnitState(UNIT_STATE_CASTING) && !me->IsWithinMeleeRange(victim))
                ++_apartSeconds;
        }
        else if (++_notEngagedSeconds >= NOT_ENGAGED_SECONDS)
            EnterEvadeMode(EVADE_REASON_NO_HOSTILES);
    }

    Fight _fight;
    bool _ready = false;
    bool _botAssisted = false;
    float _swiftRate = 0.0f;
    uint32 _secondTimer = 0;
    uint32 _regenSeconds = 0;
    uint32 _notEngagedSeconds = 0;

    std::vector<Slot> _abilities;
    uint32 _abilityTimer = 0;

    uint32 _fightSeconds = 0;
    uint32 _apartSeconds = 0;
    uint32 _controlHits = 0;
    uint32 _spellDamageTaken = 0;
    uint32 _meleeDamageTaken = 0;
    uint32 _dealtToOwner = 0;
};

bool Nemesis::SetUpCreature(Creature* creature, Fight const& fight)
{
    npc_mod_nemesis* ai = dynamic_cast<npc_mod_nemesis*>(creature->AI());
    if (!ai)
        return false;

    ai->Begin(fight);
    return true;
}

bool Nemesis::IsTalker(Creature const* creature)
{
    switch (creature->GetCreatureTemplate()->type)
    {
        case CREATURE_TYPE_HUMANOID:
        case CREATURE_TYPE_UNDEAD:
        case CREATURE_TYPE_DEMON:
        case CREATURE_TYPE_DRAGONKIN:
        case CREATURE_TYPE_GIANT:
            return true;
        default:
            return false;
    }
}

void AddNemesisCreatureScripts()
{
    RegisterCreatureAI(npc_mod_nemesis);
}
