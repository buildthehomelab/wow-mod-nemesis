/*
 * mod-nemesis: the creature itself, from the moment it appears in front of its target.
 *
 * It's a temporary summon with its own object, so nothing it gains here outlasts it: a new fight
 * starts from a new creature. Stats come from the level it fights at, not from the rare's, and it
 * fights with melee and its affixes alone: a rare's own spells are made for the rare's level.
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
#include "StringFormat.h"

#include <algorithm>

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

    uint32 const JUGGERNAUT_MECHANICS[] =
    {
        MECHANIC_CHARM, MECHANIC_DISORIENTED, MECHANIC_FEAR, MECHANIC_ROOT, MECHANIC_SLEEP, MECHANIC_SNARE,
        MECHANIC_STUN, MECHANIC_FREEZE, MECHANIC_KNOCKOUT, MECHANIC_POLYMORPH, MECHANIC_BANISH, MECHANIC_HORROR,
        MECHANIC_SAPPED,
    };

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

    // %s is the creature's name, which the client fills in.
    char const* const EMOTE_ARRIVE = "%s has tracked you down, {}!";
    char const* const EMOTE_WIN = "%s stands over {}. Its hunt is over.";
    char const* const EMOTE_DIE = "%s falls, its eyes still fixed on {}.";
    char const* const EMOTE_DIE_FINAL = "%s falls, and will not rise again.";

    // Those with a voice yell; a beast or a machine can only be described.
    bool CanTalk(Creature const* creature)
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

        ApplyAffixes();
        ApplyStats();
        KeepAuras();

        // A charmed creature runs another AI, so none of the outcomes here would happen.
        me->ApplySpellImmune(0, IMMUNITY_MECHANIC, MECHANIC_CHARM, true);

        if (Player* owner = ObjectAccessor::GetPlayer(*me, fight.owner))
        {
            Say(TALK_ARRIVE[fight.rank - 1], EMOTE_ARRIVE, owner);
            AttackStart(owner);
            me->SetAssistanceTimer(0);
        }
    }

    void Finish(Player* owner)
    {
        Say(TALK_WIN, EMOTE_WIN, owner);
    }

    void DamageTaken(Unit* attacker, uint32& damage, DamageEffectType /*type*/, SpellSchoolMask school) override
    {
        if (attacker)
            if (Player* player = attacker->GetAffectingPlayer(); player && IsBot(player))
                _botAssisted = true;

        if ((_fight.affixes & AFFIX_SPELLWARD) && !(school & SPELL_SCHOOL_MASK_NORMAL))
            damage = uint32(float(damage) * SPELLWARD_DAMAGE);
    }

    void DamageDealt(Unit* /*victim*/, uint32& damage, DamageEffectType /*type*/, SpellSchoolMask /*school*/) override
    {
        // Its rank's damage is in its stats (ApplyStats), so armor, block and shields count as
        // usual. Only this part changes during a fight, so it can't be.
        if ((_fight.affixes & AFFIX_ENRAGED) && me->HealthBelowPct(int32(ENRAGED_HEALTH_PCT)))
            damage = uint32(float(damage) * ENRAGED_DAMAGE);

        if ((_fight.affixes & AFFIX_VAMPIRIC) && damage && me->IsAlive())
            me->ModifyHealth(int32(float(damage) * VAMPIRIC_HEAL));
    }

    void KilledUnit(Unit* victim) override
    {
        if (victim->GetGUID() != _fight.owner)
            return;

        Player* owner = victim->ToPlayer();
        if (!owner)
            return;

        Finish(owner);
        OnNemesisWon(me, owner);
        me->DespawnOrUnsummon(3s);
    }

    void JustDied(Unit* /*killer*/) override
    {
        if (Player* owner = ObjectAccessor::GetPlayer(*me, _fight.owner))
        {
            if (_fight.rank >= FINAL_RANK)
                Say(TALK_DIE_FINAL, EMOTE_DIE_FINAL, owner);
            else
                Say(TALK_DIE, EMOTE_DIE, owner);
        }

        OnNemesisDied(me, _fight.owner, _botAssisted);
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

        DoMeleeAttackIfReady();
    }

private:
    void Say(char const* talk, char const* emote, Player* owner)
    {
        if (CanTalk(me))
            me->Yell(Acore::StringFormat(talk, owner->GetName()), LANG_UNIVERSAL);
        else
            me->TextEmote(Acore::StringFormat(emote, owner->GetName()));
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
            _notEngagedSeconds = 0;
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
};

bool Nemesis::SetUpCreature(Creature* creature, Fight const& fight)
{
    npc_mod_nemesis* ai = dynamic_cast<npc_mod_nemesis*>(creature->AI());
    if (!ai)
        return false;

    ai->Begin(fight);
    return true;
}

void AddNemesisCreatureScripts()
{
    RegisterCreatureAI(npc_mod_nemesis);
}
