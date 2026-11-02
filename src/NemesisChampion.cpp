/*
 * mod-nemesis: the weekly champion.
 *
 * Whoever killed their nemesis most often since the last weekly reset, in fights no bot had a part
 * in, holds a title until the next one. A tie goes to whoever got there first. It counts what's in
 * mod_nemesis_event, so a restart loses nothing, and a server that was down over a reset settles
 * that week when it comes back. A week runs from the end of the last row in mod_nemesis_champion,
 * which keeps every week: one without kills has a row with guid 0. Each week ends at the core's
 * weekly reset, so nothing drifts from it.
 *
 * The title moves at the reset for whoever is online, and at their next login for anyone else.
 * This all runs on the world thread: WorldScript::OnUpdate while the maps aren't updating, logins
 * and commands. The one query is once a week.
 *
 * Released under the MIT License.
 */

#include "Nemesis.h"
#include "Chat.h"
#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "World.h"
#include "WorldScript.h"
#include "WorldSession.h"

using namespace Nemesis;

namespace
{
    constexpr uint32 CHECK_MS = MINUTE * IN_MILLISECONDS;
    constexpr uint32 WEEK_SECONDS = 7 * DAY;

    uint32 weekStart = 0;   // when this week began
    uint32 weekEnd = 0;     // when this week ends; 0 until the table is read
    uint32 champion = 0;    // character holding the title, 0 for nobody
    uint32 checkTimer = 0;

    CharTitlesEntry const* TitleEntry()
    {
        return config.championTitle ? sCharTitlesStore.LookupEntry(config.championTitle) : nullptr;
    }

    void GiveTitle(Player* player, CharTitlesEntry const* title)
    {
        if (!player->HasTitle(title))
            player->SetTitle(title);
    }

    void TakeTitle(Player* player, CharTitlesEntry const* title)
    {
        // Clears it too if it's the one they were showing.
        if (player->HasTitle(title))
            player->SetTitle(title, true);
    }

    // The next weekly reset. The core moves on from one just after it passes, so it can still be here.
    uint32 NextReset(uint32 now)
    {
        uint32 next = uint32(sWorld->GetNextWeeklyQuestsResetTime().count());
        return next > now ? next : next + WEEK_SECONDS;
    }

    // Who won the week from `start` to `end`.
    void SettleWeek(uint32 start, uint32 end)
    {
        uint32 winner = 0;
        uint32 kills = 0;
        std::string name;

        if (QueryResult result = CharacterDatabase.Query(
            "SELECT e.guid, c.name, COUNT(*) AS kills, MAX(e.at) AS last_at "
            "FROM mod_nemesis_event e JOIN characters c ON c.guid = e.guid "
            "WHERE e.event IN (3, 4) AND e.bots = 0 AND e.at >= {} AND e.at < {} AND c.deleteDate IS NULL "
            "GROUP BY e.guid, c.name ORDER BY kills DESC, last_at LIMIT 1", start, end))
        {
            Field* fields = result->Fetch();
            winner = fields[0].Get<uint32>();
            name = fields[1].Get<std::string>();
            kills = uint32(fields[2].Get<uint64>());
        }

        std::string escaped = name;
        CharacterDatabase.EscapeString(escaped);
        CharacterDatabase.Execute("REPLACE INTO mod_nemesis_champion (week_end, guid, name, kills) VALUES ({}, {}, '{}', {})", end, winner, escaped, kills);

        uint32 previous = champion;
        champion = winner;

        if (CharTitlesEntry const* title = TitleEntry())
        {
            if (previous && previous != winner)
                if (Player* player = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(previous)))
                    TakeTitle(player, title);

            if (winner)
                if (Player* player = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(winner)))
                    GiveTitle(player, title);
        }

        LOG_INFO("module", "mod-nemesis: week ending {} won by character {} with {} kills", end, winner, kills);

        if (winner)
            ChatHandler(nullptr).SendWorldText("|cffff4040Nemesis:|r {} is this week's Nemesis Hunter, with {} kills!", name, kills);
    }

    void LoadWeek()
    {
        champion = 0;

        uint32 now = uint32(GameTime::GetGameTime().count());
        weekEnd = NextReset(now);
        weekStart = weekEnd - WEEK_SECONDS;

        if (QueryResult result = CharacterDatabase.Query("SELECT week_end, guid FROM mod_nemesis_champion ORDER BY week_end DESC LIMIT 1"))
        {
            Field* fields = result->Fetch();
            weekStart = fields[0].Get<uint32>();
            champion = fields[1].Get<uint32>();

            // The server was down over a reset: that week gets settled now.
            if (weekStart + WEEK_SECONDS <= now)
                weekEnd = weekStart + WEEK_SECONDS;
        }
    }
}

bool Nemesis::EndWeekNow()
{
    if (!config.enabled || !config.championEnabled)
        return false;

    weekEnd = uint32(GameTime::GetGameTime().count());
    return true;
}

class NemesisChampionWorldScript : public WorldScript
{
public:
    NemesisChampionWorldScript() : WorldScript("NemesisChampionWorldScript", { WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE }) { }

    void OnStartup() override
    {
        LoadWeek();
    }

    void OnUpdate(uint32 diff) override
    {
        if (!config.enabled || !config.championEnabled || !weekEnd)
            return;

        checkTimer += diff;
        if (checkTimer < CHECK_MS)
            return;
        checkTimer = 0;

        uint32 now = uint32(GameTime::GetGameTime().count());
        if (now < weekEnd)
            return;

        SettleWeek(weekStart, weekEnd);
        weekStart = weekEnd;
        weekEnd = NextReset(now);
    }
};

class NemesisChampionPlayerScript : public PlayerScript
{
public:
    NemesisChampionPlayerScript() : PlayerScript("NemesisChampionPlayerScript", { PLAYERHOOK_ON_LOGIN }) { }

    // Who was offline when the title moved, and everyone if the champion was switched off.
    void OnPlayerLogin(Player* player) override
    {
        CharTitlesEntry const* title = TitleEntry();
        if (!title)
            return;

        if (config.enabled && config.championEnabled && player->GetGUID().GetCounter() == champion)
            GiveTitle(player, title);
        else
            TakeTitle(player, title);
    }
};

void AddNemesisChampionScripts()
{
    new NemesisChampionWorldScript();
    new NemesisChampionPlayerScript();
}
