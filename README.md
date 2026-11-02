# mod-nemesis

Kill a rare out in the world, and there's a small chance it isn't finished with you.

It comes back as a copy of itself with a new name, "Hogger the Vengeful", bigger and wrapped in a
visual aura, and it hunts you across zones and continents. Each time you kill it, it returns a rank
stronger, having learned from how you beat it. If it kills you, it has had its revenge: the hunt is
over and your streak with it. Kill it at the last rank and it's gone for good, with a title, the best
reward and a chance at a mount.

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
- **It learns:** how you beat it decides what it comes back with. Beaten mostly with spells, it
  returns Spellward. Stunned, feared or rooted, Juggernaut. Kept out of reach, Swift. If it dealt
  far more than your health and still lost, Savage. It tells you so when it arrives.
- **It warns you:** you get the direction it's coming from when it picks up your trail, a whisper
  from afar, and a message on screen with a sound when it's close.
- **Rewards:** the rare's own loot table and gold by level and rank. At the end: double gold, the
  Feat of Strength "Bane of the Undying" with its title, and a chance at a mount chosen by creature
  type: 25%, and 25% more for each roll your account has missed. Only the hunted player gets the
  Feat and rolls for the mount, and the roll only happens if no bot was in the group or hit it at
  any point during the fight. Bots don't cost you the Feat. Other real players can help.
  A rare whose loot can drop a mount has copies with no loot at all, since a copy can be killed
  again and again.
- **Someone else kills it:** nothing for you, and it comes back later at the same rank.
- **Voices:** humanoids, undead, demons, dragonkin and giants yell at you by name. Beasts,
  elementals and machines get an emote.

- **Weekly champion:** whoever kills their nemesis most often between weekly resets, in fights no
  bot had a part in, is announced to the server and holds the title "the Nemesis Hunter" until the
  next reset. Ties go to whoever got there first. The title moves at the reset, or at the next login
  for anyone offline.

Affixes: Vampiric, Swift, Juggernaut, Savage, Spellward, Enraged and Regenerating.

Abilities come from the creature type, or a caster's set if the rare was a mage, and open at ranks
1, 3 and 5: charges, fears, stuns, bolts and bleeds, with damage worked out from its melee hit so
they stay a threat at any level. It doesn't use the rare's own spells, which are made for the rare's
level.

What happens to each hunt goes into the characters table `mod_nemesis_event` and the server log, for
tuning: how often it triggers, what it learns, how far players get, how long fights last, and whether
bots helped.

## Install

```bash
cd modules
git clone https://github.com/buildthehomelab/wow-mod-nemesis.git mod-nemesis
```

Rebuild the server. The SQL is applied on the next start. Then copy
`conf/mod_nemesis.conf.dist` to your config folder as `mod_nemesis.conf` if you want to change it.

The nemesis itself needs no client patch: the client asks the server for the names of new creature
entries. The Feat and titles do: run `tools/build_patch.py` on your server's client patch, or set
`Nemesis.Final.Achievement = 0` and `Nemesis.Champion.Title = 0`.

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
| `.nemesis endweek` | GM | settles the weekly champion within a minute |
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
- It learns from how you beat it, and it comes back ready. Rely on spells and it returns
  **Spellward**. Stun or fear it and it returns a **Juggernaut**. Kite it and it returns **Swift**.
- It fights with abilities of its own: charges, fears, stuns, bolts and bleeds, more of them at
  higher ranks.
- You get a warning of the direction it's coming from as it closes in.
- If your nemesis kills you, it has had its revenge. The hunt is over, and your streak with it.
- At **Mythic** it fights as an elite at your level cap. Beat it to end the hunt for good and earn
  the Feat of Strength **Bane of the Undying** and its title, with a chance at a mount if no bots
  helped. Each time you miss the mount, your next chance improves. It holds back until you are
  close to your level cap.
- Each week, whoever kills their nemesis most often without bots earns the title
  **the Nemesis Hunter** until the next weekly reset.

> Nothing about a nemesis is on a timer you have to race. If you can't fight it now, reach a town
> and it will wait.

## Uninstall

Remove the module, rebuild, and run the two scripts in `data/sql/uninstall/` by hand.

Released under the MIT License.
