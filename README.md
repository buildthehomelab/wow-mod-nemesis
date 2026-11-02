# mod-nemesis

Kill a rare out in the world, and there's a small chance it isn't finished with you.

It comes back as a copy of itself with a new name, "Hogger the Vengeful", bigger and wrapped in a
visual aura, and it hunts you across zones and continents. Each time you kill it, it returns a rank
stronger. If it kills you, it has had its revenge: the hunt is over and your streak with it. Kill it
at the last rank and it's gone for good, with the best reward and a chance at a mount.

## How it plays

| Rank | Name | Level | Health × | Damage × | Size × | Affixes |
|---|---|---|---|---|---|---|
| 1 | Marked | yours | 1.5 | 1.25 | 1.2 | 1 |
| 2 | Hated | yours | 2 | 1.5 | 1.3 | 1 |
| 3 | Relentless | yours | 2.5 | 2 | 1.4 | 2 |
| 4 | Legendary | yours | 3 | 2.5 | 1.5 | 2 |
| 5 | Mythic ("the Undying") | your cap | elite | elite | 1.6 | 3 |

- **Starting:** each real player credited with a rare's kill rolls 5%. One nemesis per character.
  Bots and Hardcore characters (mod-challenges) never get one, nor does anyone 10 or more levels
  above the rare.
- **The hunt:** it waits 30 to 60 minutes of play, then travels towards you a little faster than an
  epic ground mount. Crossing to another continent takes it 5 minutes. No creature exists until it
  reaches you, so a hunt costs next to nothing. It only moves while you're online.
- **Where it won't appear:** capitals, towns, inns, sanctuaries or near guards, or while you're in
  a dungeon, flying, on a flight path, in water, dead, in a duel or already in combat. It waits
  close by and appears as soon as it can.
- **Getting away:** run 150 yards, reach a safe place, or lose it (vanish, feign death), and it
  goes back to following, without penalty.
- **The last rank:** an elite at the highest level you can reach now: your progression phase's cap
  with mod-individual-progression (60, 70 or 80), the server's otherwise. It stays at rank 4 until
  you're within 5 levels of that cap, and rank 4 kills keep paying.
- **Rewards:** the rare's own loot table and gold by level and rank. At the end: double gold and a
  25% chance at a mount, chosen by creature type. Only the hunted player rolls for it, and only if
  no bot was in the group or hit it at any point during the fight. Other real players can help.
  A rare whose loot can drop a mount has copies with no loot at all, since a copy can be killed
  again and again.
- **Someone else kills it:** nothing for you, and it comes back later at the same rank.
- **Voices:** humanoids, undead, demons, dragonkin and giants yell at you by name. Beasts,
  elementals and machines get an emote.

Affixes: Vampiric, Swift, Juggernaut, Savage, Spellward, Enraged and Regenerating.

A nemesis fights with melee and its affixes only. It doesn't use the rare's spells, which are made
for the rare's own level.

## Install

```bash
cd modules
git clone https://github.com/buildthehomelab/wow-mod-nemesis.git mod-nemesis
```

Rebuild the server. The SQL is applied on the next start. Then copy
`conf/mod_nemesis.conf.dist` to your config folder as `mod_nemesis.conf` if you want to change it.

No client patch is needed: the client asks the server for the names of new creature entries.

**Bots:** with mod-playerbots, install [mod-rare-tracker](https://github.com/buildthehomelab/wow-mod-rare-tracker)
with `RareTracker.BotsIgnoreRares = 1`. It keeps free-roaming bots off anything of rare rank, which
includes these copies.

**Mounts:** the default `Nemesis.Mount.*` items come from a retail mount pack. Set them to items
your server has. An item the server doesn't have means no mount for that type.

**Creatures:** `data/sql/db-world/mod_nemesis_world.sql` holds entries 9600000-9601999 and is
generated. To cover your own rares, run `tools/build_creatures.py` against your world database:
entries already in the file keep their numbers.

## Commands

| Command | Who | What |
|---|---|---|
| `.nemesis` | everyone | shows your nemesis, if you have one |
| `.nemesis start <rare entry> [rank]` | GM | gives you a nemesis from that rare |
| `.nemesis rank <rank>` | GM | sets its rank |
| `.nemesis now` | GM | ends its wait and puts it a short way from you |
| `.nemesis clear` | GM | ends your hunt |
| `.nemesis reload` | admin | reads the config and the creature table again |

A nemesis doesn't appear to a game master, so use `.gm off` to meet it. Rank 5 drops back to 4
unless you're within 5 levels of your cap.

## Patch Notes: Nemesis

Category: World

- Rares don't always stay dead. Killing one in the open world now has a small chance of making it
  your **Nemesis**. It returns under a new name and hunts you, even across zones and continents.
- A nemesis won't come for you in a capital, a town or an inn, or near guards. It waits nearby.
- Each time you kill it, it comes back stronger and pays more gold, through five ranks: **Marked**,
  **Hated**, **Relentless**, **Legendary** and **Mythic**.
- If your nemesis kills you, it has had its revenge. The hunt is over, and your streak with it.
- At **Mythic** it fights as an elite at your level cap. Beat it to end the hunt for good, with a
  chance at a mount if no bots helped. It holds back until you are close to your level cap.

> Nothing about a nemesis is on a timer you have to race. If you can't fight it now, reach a town
> and it will wait.

## Uninstall

Remove the module, rebuild, and run the two scripts in `data/sql/uninstall/` by hand.

Released under the MIT License.
