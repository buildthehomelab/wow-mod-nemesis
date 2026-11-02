/*
 * mod-nemesis: commands.
 *
 *   .nemesis                        your nemesis, if you have one
 *   .nemesis start <rare> [rank]    (GM) gives you a nemesis from that rare's creature entry
 *   .nemesis rank <rank>            (GM) sets your nemesis's rank
 *   .nemesis now                    (GM) ends its wait and puts it a short way from you
 *   .nemesis clear                  (GM) ends your hunt
 *   .nemesis endweek                (GM) settles the weekly champion within a minute
 *   .nemesis reload                 (admin) reads the config and the creature table again
 *
 * A nemesis doesn't appear to a game master: use .gm off to meet it. Rank 5 drops back to 4 unless
 * you are within Nemesis.Final.HoldLevels of your level cap.
 *
 * Released under the MIT License.
 */

#include "Nemesis.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "Player.h"
#include "ScriptMgr.h"

using namespace Acore::ChatCommands;
using namespace Nemesis;

class NemesisCommandScript : public CommandScript
{
public:
    NemesisCommandScript() : CommandScript("NemesisCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable nemesisTable =
        {
            { "",       HandleStatus, SEC_PLAYER,        Console::No },
            { "start",  HandleStart,  SEC_GAMEMASTER,    Console::No },
            { "rank",   HandleRank,   SEC_GAMEMASTER,    Console::No },
            { "now",    HandleNow,    SEC_GAMEMASTER,    Console::No },
            { "clear",  HandleClear,  SEC_GAMEMASTER,    Console::No },
            { "endweek", HandleEndWeek, SEC_GAMEMASTER,  Console::No },
            { "reload", HandleReload, SEC_ADMINISTRATOR, Console::Yes },
        };

        static ChatCommandTable commandTable =
        {
            { "nemesis", nemesisTable },
        };

        return commandTable;
    }

    static bool HandleStatus(ChatHandler* handler)
    {
        Status status = StatusOf(handler->GetSession()->GetPlayer());
        if (!status.found)
        {
            handler->PSendSysMessage("Nothing hunts you. Yet.");
            return true;
        }

        handler->PSendSysMessage("{} hunts you: rank {} {} ({}), {} kills so far.", status.name, uint32(status.rank), RankName(status.rank), AffixNames(status.affixes), status.kills);
        if (status.engaged)
            handler->PSendSysMessage("It has found you.");
        else if (status.waitSeconds)
            handler->PSendSysMessage("It is biding its time, for now.");
        else if (!status.direction.empty())
            handler->PSendSysMessage("It is on your trail, to the {}.", status.direction);
        else
            handler->PSendSysMessage("It is on your trail.");
        return true;
    }

    static bool HandleStart(ChatHandler* handler, uint32 rareEntry, Optional<uint8> rank)
    {
        if (!StartHunt(handler->GetSession()->GetPlayer(), rareEntry, rank.value_or(1)))
            handler->PSendSysMessage("You already have a nemesis, or creature {} is not a rare with copies.", rareEntry);
        return true;
    }

    static bool HandleRank(ChatHandler* handler, uint8 rank)
    {
        if (SetRank(handler->GetSession()->GetPlayer(), rank))
            handler->PSendSysMessage("Your nemesis is now rank {}.", uint32(StatusOf(handler->GetSession()->GetPlayer()).rank));
        else
            handler->PSendSysMessage("You have no nemesis, or it's in the world right now.");
        return true;
    }

    static bool HandleNow(ChatHandler* handler)
    {
        if (HuntNow(handler->GetSession()->GetPlayer()))
            handler->PSendSysMessage("It sets out. Use .gm off if you are in GM mode.");
        else
            handler->PSendSysMessage("You have no nemesis, it's in the world right now, or you aren't in the open world.");
        return true;
    }

    static bool HandleClear(ChatHandler* handler)
    {
        if (ClearHunt(handler->GetSession()->GetPlayer()))
            handler->PSendSysMessage("Hunt ended.");
        else
            handler->PSendSysMessage("You have no nemesis.");
        return true;
    }

    static bool HandleEndWeek(ChatHandler* handler)
    {
        if (EndWeekNow())
            handler->PSendSysMessage("The week ends now. The champion is settled within a minute, and the next one runs to the weekly reset.");
        else
            handler->PSendSysMessage("The weekly champion is turned off.");
        return true;
    }

    static bool HandleReload(ChatHandler* handler)
    {
        sConfigMgr->LoadModulesConfigs(true, false);
        LoadConfig();
        ReloadCopies();
        handler->PSendSysMessage("mod-nemesis: config and creature table read again.");
        return true;
    }
};

void AddNemesisCommandScripts()
{
    new NemesisCommandScript();
}
