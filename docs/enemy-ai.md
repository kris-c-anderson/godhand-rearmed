# How God Hand's enemies decide to attack

Read from the retail code (SLUS_215.03). Addresses are of the boot ELF; names come from the
[decomp](https://github.com/LucasPicoli/god-hand-decomp) where it has one. Distances are in metres (the game's units),
angles in degrees. The game logic runs at 30 updates a second; times below are in seconds.

## No camera involved

The only camera test the enemies have (`cGameObj_isClip`, `IsTargetVisible_14B470` and the per-class copies such as
`func_00279F00`, all virtual slot 0x30) is called by `cModel_Trans` and `func_00319520`: it decides whether a model is
drawn, and whether its skeleton is animated (`+0x250` bit 0x200000). Nothing in the enemies' decisions reads it. They
go by distances to Gene and by **the way Gene faces**. The aspect ratio and the field of view therefore do not change
how enemies fight; a wider view only shows the enemies that, on the PS2's 4:3 picture, waited just past its edges.

## What every enemy works out each update

`func_00208BE0` (the common enemy update) stores, per enemy:

| Field | Meaning |
|---|---|
| `+0x75C` / `+0x760` | angle from the enemy's own facing to Gene (signed / absolute) |
| `+0x764` / `+0x768` | angle from **Gene's facing** to the enemy (signed / absolute) |
| `+0x618` | squared distance to Gene (on the floor) |
| `+0x16D8` | its place in the queue (below) |

## 1. The queue

`func_0020F3C0` calls `cEmManage_countEmNearerPlayer`: the number of other enemies closer to Gene (within 2 m of his
height). That is the enemy's place, 0 = closest (`+0x16D8`). Some kinds add to it, and in the Dark World every enemy
counts 3 places further back.

On Level 1 an enemy in second place for about 5 s (150 updates, `+0x175C`) acts as if first for 3 s (`+0x1760`); on
Level 2 the same for third place.

It also gives the enemy a spot to walk to (`+0x16A0`): one of 3 (Levels 1-2), 5 (Level 3) or 7 (Level DIE) points
around Gene, in his own frame: (0, 0), (±1, 1), (±1.5, 1.5), (±2, 2), by the enemy's number. The spot is kept only
when the way there is clear (`ChkLine`, `func_002A7CA0`), flag `+0x16D4` bit 0x08000000.

## 2. How close it may come (`func_00211C08`)

The common enemy's moves are sub-steps (`+0x2F5`) run from `func_0020F3C0` through the table at `0x3BF1C8`; sub-step 0,
`func_00211C08`, is the idle "think" every common enemy comes back to. Bosses and a few special kinds have their own
(`cEm00_idleChooseNext` and others), with their own rules.

| Place | Level 1 | Level 2 | Level 3, DIE |
|---|---|---|---|
| 0 | 1.8 | 1.8 | 1.8 |
| 1 | 5 | 1.8 | 1.8 |
| 2 | 5 | 5 | 1.8 |
| 3-5 | 8 | 8 | 8 |
| 6 and on | 12 | 12 | 12 |

(the table at `D_00435270`, indexed by `cCoreSave_getGameLevel() - 1`).

**The wedge:** an enemy in places 0-2 that is 60° or more to Gene's side (`+0x768 >= 1.0472`, at `0x2127BC`, on every
level) never counts as having arrived: it keeps walking toward its spot. Within 4 m of Gene it circles him instead
(`0x2126B0`, sub-step 9 `func_002147D0`, the direction from the side it is on). That is the drift toward his
front.

Once an enemy has arrived: inside 3 m, every enemy past the first 1 (Level 1), 2 (Level 2) or 3 (Level 3) places
steps back (sub-step 7 `func_00213EA8`, table `D_00435290`); at Level DIE none does. Once every 8 updates it also checks for another
enemy in the way (`func_00291010`, 60° either side of its own facing) and steps round it.

## 3. The attacks (`func_00262AA8`, run first, every update)

Per enemy kind, before the queue logic:

- **Point blank:** within 2.5 m (2 m on Level 1), a random attack. No facing check.
- **Lunges** from 3-4 m (some kinds 4-5 m, a few further): only within a cone of Gene's facing, by level
  (`D_00444990` and its copies): none on Level 1, ±45° on Level 2, ±60° on Level 3, **±90°** at Level DIE; then a
  10 s wait (`+0x16E0` = 300).

## Difficulty and the level gauge

`cCoreSave_getGameLevel` turns the gauge's points into Level 1, 2, 3 or DIE (5); the thresholds are 1000, 3000, 4000
and 5000 points (`0x3BF160`; the fourth level is skipped). `cCoreSave_getGameDifficulty`: 0 Easy, 1 Normal, 2 Hard.

- **Easy:** the gauge cannot pass Level 2 (points capped at 2999).
- **Normal:** starts at Level 1, can reach DIE.
- **Hard:** starts at Level DIE (`cCoreSave_gameInit`) and the gauge never moves (`cCoreSave_addGameLevelPoint` and
  `cCoreSave_dropLevelPoint` return at once for difficulty 2).

So Hard is Level DIE for the whole game, and Normal at DIE plays the same: the lunge cone covers Gene's whole front
half, the first three come in to 1.8 m, and nobody steps back.

## Where a change would go

- Widen the 60° wedge: the compares at `0x2126C4` and `0x2127D0` in `func_00211C08` (`lui at,0x3F86` / `ori
  at,0x0A92` = 1.0472 just before each).
- Widen the lunge cones: the per-level angle tables read in `func_00262AA8` (`D_00444990`, `D_00444AD0` and the other
  blocks that read `+0x768`).
- Use the real camera instead of the wedge: hooks at those compares that test the enemy against the camera's view
  instead of the angle.
