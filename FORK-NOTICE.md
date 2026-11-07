# Fork notice

This is a modified version of [Lichii](https://github.com/zortos293/Lichii), itself based on Eden and
Yuzu. It remains under the GNU General Public License v3.0 or later: see `LICENSE.txt` and `LICENSES/`.
Upstream copyright and license headers are unchanged.

## What this fork changes

Based on Lichii `nx2`, modified by Nolar-Hack for The Legend of Zelda: Ocarina of Time on Linux with
AMD GPUs (RADV). Tested on one machine only: i5-12400, RX 7900 XTX, game build
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`. It may not work elsewhere.

- Eglantine #609: native compute storage buffers are sized from the mapping only for DLSS and pointer
  tables. On this machine it removed the heavy overexposure in Kokiri Forest.
- RADV depth-compare fix and a texture collector threshold (`EDEN_NX2_TC_MINIMUM_MIB`).
- `EDEN_NX2_SWAP_INTERVAL` is lifted while a video plays, so movies keep their speed under a 30 FPS cap.
- Dynarmic A64 superblocks and register allocation changes.
- Ported from other forks: guest RAM release on stop (SmileyTatsu), Vulkan scheduler ownership in the
  split GPU thread (rdenno), and upstream PR #214 tools (dotaku22).

Suggested launch settings for a 30 FPS cap: `EDEN_NX2_SWAP_INTERVAL=2`, with Dynamic FPS enabled.
The full list of changes is in the Git history. Parts of this work were written with Claude (Anthropic).

## Not included

This repository contains no game, firmware or keys. You must dump your own from hardware you own.
This fork is not affiliated with Nintendo or with the upstream Lichii project.
