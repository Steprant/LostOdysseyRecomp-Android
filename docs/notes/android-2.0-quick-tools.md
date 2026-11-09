# Quick tools: EXP ×3/×4 and 0.75× encounters (Steprant 1.9 / 2.0)

Requested by Steprant on 2026-11-09.

## Battle EXP

- The Quick tools row **Battle EXP** cycles OFF, ×2, ×3, ×4 (Left/Right/Confirm).
- The hook is unchanged in place: the victory calculator `sub_82AC1DC8` (return
  `0x82AC6E14`) produces each character's 0..100 award; the award is multiplied
  and kept at the game's 100 cap, because the game's own application grants at
  most one level and discards overflow. SP, gold and skills are untouched.
- `settings.ini`: `battle_exp_multiplier=1..4`. Saving also writes
  `battle_exp_double=1` for ×2..×4, so a 1.8 build reading the same profile keeps
  its ×2 switch. A profile with only `battle_exp_double=1` reads as ×2.
- Pure helpers in `settings/battle_exp_multiplier.h` (`ScaledBattleExp`).

## Encounter rate 0.75×

- New value between Normal and Half. Stored as `encounter_rate=3` (the existing
  values 0 Normal, 1 Half, 2 None keep their meaning); menus show the order
  Normal, 0.75×, Half, None, Every step.
- Same mechanism as Half: only successful walking draws are counted; 0.75× skips
  the first of every four (Half skips every other). Scripted battles, boss fights,
  battle-tour draws and Every step are not affected.
- Pure rule in `settings/encounter_rate.h` (`EncounterDrawAllowed`).

## Tests (host)

- `tools/tests/settings_menu`: `battle_exp_hook` (×1..×4 on synthetic guest
  memory, the full 0..120 × 0..5 table, persistence and failure),
  `battle_exp_startup_on/x3/x4`, `encounter_config` (100 successful draws give
  100/75/50/0 battles, restart after a mode change, migration of stored values),
  `debug_menu_interaction` and `debug_overlay` (row cycling, rendering in both
  languages, value widths).
- Not yet verified in the game on the phone.
