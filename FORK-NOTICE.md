# Fork notice

This is a modified version of [Lichii](https://github.com/zortos293/Lichii), itself based on Eden and
Yuzu. It remains under the GNU General Public License v3.0 or later: see `LICENSE.txt` and `LICENSES/`.
Upstream copyright and license headers are unchanged.

## What this fork changes

Modified from 2026-11-05 onward by Nolar-Hack, for The Legend of Zelda: Ocarina of Time on Linux with
AMD GPUs (RADV). Tested on one machine only: i5-12400, RX 7900 XTX, game build
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`. It may not work elsewhere.

- Open upstream pull requests merged early: #161, #175, #176, #178, part of #149 and #166.
- Ports from Eglantine (#609) and upstream experimental branches.
- Local changes: dynarmic A64 superblocks, texture collector threshold (`EDEN_NX2_TC_MINIMUM_MIB`),
  GOB-based unswizzle, UMWAIT waits, RADV depth-compare fix, split GPU deadlock fix, and a switch that
  wakes the game's frame rate controller (`EDEN_NX2_OOT_VFR`).

The full list of changes is in the Git history. Parts of this work were written with Claude (Anthropic).

## Not included

This repository contains no game, firmware or keys. You must dump your own from hardware you own.
This fork is not affiliated with Nintendo or with the upstream Lichii project.
