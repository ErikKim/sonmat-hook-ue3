# sonmat-hook-ue3

A part of 손맛 햅틱 / Sonmat Haptics (`sonmat`): dshaptics_ue3_x86.dll.

Hook for 32-bit Unreal Engine 3 games (Batman: Arkham Asylum): UAudioComponent::Play / Cleanup and FName::ToString found by byte patterns in sigs_ue3.txt beside the DLL (the app writes it from the game's hook.json). One line per sound into the event file the engine reads, the same line the UE4 hook writes; Bink movies opening and closing are lines of their own.

The app does not carry this in its installer for good: it fetches the release zip that `sonmat-games/parts.json` names
(address + SHA-256) into `<app>/parts/hook-ue3/` when a game needs it (`sonmat/tools/partfetch.py`). A change here reaches
players when a new tag has been released and `parts.json` points at it.

Build: Visual Studio C++ tools (x86), MinHook in minhook\ at the commit the workflow pins: build.bat dshaptics_ue3 x86. Every push to main builds it on GitHub (artifact); a tag `v*` makes the release.

MinHook (Tsuda Kageyu, BSD 2-Clause) is compiled in; its license is in the release zip as `MinHook-LICENSE.txt`.
