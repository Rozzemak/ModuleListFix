# ModuleListFix

**ReShade add-ons (and other mod loaders) fail with error `1114` in a process that hides its own module list. This DLL puts the module list back.**

A small Windows DLL you load before ReShade. No configuration per game, no patching of the executable, no rebuild of the add-ons themselves: the add-ons you already have start loading. If effects already render and only add-ons fail, this is the fix.

Prebuilt DLLs: see [Releases](https://github.com/Rozzemak/ModuleListFix/releases) - `ModuleListFix-x64.zip` and `ModuleListFix-x86.zip`, each with the DLL, a placeholder `ReShade.ini` and a `readme.txt`.

---

## Install

1. Put `ModuleListFix.dll` in the game folder - the same folder as the game `.exe` and `ReShade.ini`.

2. Add one line to `ReShade.ini`, under `[ADDON]`:

```ini
[ADDON]
LoadFromDllMain=ModuleListFix.dll
```

   - `LoadFromDllMain` takes a **file name only**, no path - the file is looked up next to the executable. Rename the DLL if you like and put the new name here.
   - If your `ReShade.ini` **already has** an `[ADDON]` section, add only the `LoadFromDllMain` line to it. Do not add a second `[ADDON]` header.
   - If it has a `LoadFromDllMain` line pointing at an add-on file that no longer exists, replace it with `ModuleListFix.dll`; that stale line is what produces the harmless `error code 126`.

3. Start the game.

Pick the DLL that matches the game: **x64** for any 64-bit game (nearly all of them), **x86** only for a 32-bit game. Mixing them fails with `193`.

To disable it later: delete the DLL, or remove the `LoadFromDllMain` line. A missing file only logs a harmless `126` in `ReShade.log`.

### If another DLL already uses that slot

`LoadFromDllMain` is not single-valued: ReShade parses it as a list and also appends repeated keys, so put both in and let ModuleListFix go first (it has to patch before the other DLL runs):

```ini
LoadFromDllMain=TheirLoader.dll,ModuleListFix.dll
```

which is the same as:

```ini
[ADDON]
LoadFromDllMain=TheirLoader.dll
LoadFromDllMain=ModuleListFix.dll
```

If the host only has a single slot, ModuleListFix can chain-load the displaced DLL itself. Create `ModuleListFix.ini` next to `ModuleListFix.dll`:

```ini
[ModuleListFix]
Load=TheirLoader.dll
```

One entry per line, bare name or `Load=name`, relative to the DLL's folder (an absolute path also works). Entries are loaded **after** the patch is installed, with the same search flags ReShade uses. Failures are logged, not fatal.

### Any other host

Anything else that needs module enumeration works the same way: `LoadLibrary` this DLL early from a proxy DLL you already control, before the dependent component initialises.

---

## Checking that it took

`ModuleListFix.log` appears next to the DLL:

```log
[proc 2820 15:23:21.905] === attach pid=2820 module=D:\Games\SomeGame\ModuleListFix.dll
[proc 2820 15:23:21.910] before patch: K32EnumProcessModules ok=0 needed=0 err=6
[proc 2820 15:23:21.911] patch: K32EnumProcessModules installed at 00007FFE84F034A0
[proc 2820 15:23:21.911] patch: K32EnumProcessModules installed at 00007FFE83B6B8E0
[proc 2820 15:23:21.912] patch: total_installed=6
[proc 2820 15:23:21.912] after patch: K32EnumProcessModules ok=1 count=2048
[proc 2820 15:23:26.298] call 3: cb=8192 walked=2048 returned=1024
```

And in `ReShade.log`, the line that matters:

```log
Registered add-on "DLSS 5 Neural Rendering" v0.2026.1020.1459 using ReShade API version 18.
Registered add-on "RenoDX" v0.2026.1111.2326 using ReShade API version 18.
Registered add-on "ReLimiter" v3.3.5.0 using ReShade API version 18.
```

Before the fix the same three add-ons produced `1114` on all four load attempts, every launch.

---

## The failure

ReShade itself works: hooks install, the swap chain is created, effects compile and render. Only add-ons die, every time:

```log
Loading add-on from 'D:\Games\SomeGame\renodx-ue-extended.addon64' ...
ERROR | Failed to load add-on from 'D:\Games\SomeGame\renodx-ue-extended.addon64' with error code 1114!

Loading add-on from 'D:\Games\SomeGame\renodx-dlss5.addon64' ...
ERROR | Failed to load add-on from 'D:\Games\SomeGame\renodx-dlss5.addon64' with error code 1114!

Loading add-on module "D:\Games\SomeGame\renodx-dlss.addon64" ...
ERROR | Failed to load add-on from 'D:\Games\SomeGame\renodx-dlss.addon64' with error code 126!
```

| Code | Windows meaning | What it is telling you |
|---|---|---|
| `1114` | `ERROR_DLL_INIT_FAILED` | `LoadLibrary` reached the add-on, its `DllMain` ran, and `DllMain` returned `FALSE`. Nothing in the file is corrupt. **This is the bug this project fixes.** |
| `126` | `ERROR_MOD_NOT_FOUND` | The file named in `[ADDON] LoadFromDllMain` does not exist - usually a leftover after renaming an add-on. Harmless noise. |
| `193` | `ERROR_BAD_EXE_FORMAT` | 32/64-bit mismatch. Not this bug. |

The add-on's own log agrees it barely started:

```log
[session] v10.8.0 pid=14908 ends=logged exe=Game.exe
[exit] v10.8.0 pid=14908 up_ms=79 unload
[exit] v10.8.0 pid=14908 up_ms=94 unload
```

And unrelated in-process tooling fails the same way, which is the giveaway that the process - not the add-on - is broken:

```log
UE4SS: Fatal Error: Was unable to enumerate game modules. Error: [0x6] The handle is invalid.
```

---

## What is actually broken

`1114` is not a ReShade version problem, not an API version problem, and not a corrupted file. Modern add-ons ship ReShade's `reshade.hpp`, which locates the ReShade core like this:

```cpp
HMODULE modules[1024]; DWORD needed = 0;
K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed);
// take the first module that exports ReShadeRegisterAddon + ReShadeUnregisterAddon
```

Measured inside such a process, with a diagnostic build that logs the API result:

```log
GetModuleHandleW(reshade.asi)=00007FFDC09D0000      <- ReShade IS loaded and findable
before patch: K32EnumProcessModules ok=0 needed=0 err=6
after patch:  K32EnumProcessModules ok=1 count=2048
```

`err=6` is `ERROR_INVALID_HANDLE` and `needed=0` means **zero modules**. The enumeration is dead in that process, so the add-on's lookup returns null, its registration helper returns `false`, `DllMain` returns `FALSE`, and the loader reports `1114`.

Why is it dead? Wrappers that must be invisible to integrity checks - manual mapping, protectors, some anti-debug layers, hypervisor-assisted loaders - unlink or corrupt their own entries in the PEB loader lists. The process runs normally, but the loader data no longer validates, and kernel32's enumeration gives up. The same reason makes the main executable appear in crash reports as `faulting module: unknown, version 0.0.0.0`.

Walking the PEB list directly still works there. That is the whole fix.

---

## What ModuleListFix does

1. Walks `PEB->Ldr->InLoadOrderModuleList` to build the real module list.
2. Replaces `K32EnumProcessModules` in **kernel32**, **kernelbase** and **psapi** (`EnumProcessModules`, `...Ex`) with that walk.
3. Clamps the result to the caller's buffer and returns success.
4. Runs a watchdog thread that re-applies any patch a protection layer reverts.

Two details decide whether this works at all, both learned the hard way:

- **Patch kernelbase, not just kernel32.** The loader binds imports straight to the implementation inside `KernelBase.dll`, so an add-on whose import table is already resolved never reaches the kernel32 forwarder.
- **Clamp instead of reporting a short buffer.** Callers pass a fixed 8 KB array (1024 handles). Returning `FALSE` with `*needed = 16384` - what the real API does - is treated as failure by those callers. Fill what fits and return `TRUE`.

Nothing else is hooked.

---

## Is this your bug?

You need this if all of the following are true:

- ReShade itself works (effects render, overlay opens) but **every** add-on fails with `1114`;
- the game ships unusual loader shims (proxies for system DLLs, a custom loader or emulator layer) or runs under a hypervisor-level protection;
- other in-process tooling that enumerates modules also fails - `UE4SS`, trainers, overlays, script loaders.

You do **not** need it for `193` (bitness), `126` (missing file or dependency), or when only one specific add-on fails while others load.

---

## Build

MSVC. Open the **Native Tools Command Prompt** that matches the target - "x64 Native Tools" for the
64-bit DLL, "x86 Native Tools" for the 32-bit one - then:

```bat
build.bat
```

which is just:

```bat
cl /nologo /LD /EHsc /O2 /Fe:ModuleListFix.dll src\ModuleListFix.cpp
```

(the prompt decides the architecture; `/MACHINE:X64` or `/MACHINE:X86` can be added explicitly).

The output file name does not matter; load it under whatever name your host expects.

---

## Caveats

- **User mode only.** If the loader data is gone rather than merely inconsistent, there is nothing to walk.
- **Some protections are aggressive.** If `ModuleListFix.log` fills with `watchdog: re-patched site`, the patch is being reverted faster than it is applied and that process is not a good candidate.
- **Hooking a system DLL is invasive by nature.** It is safe here because the replacement returns the same shape of data the original would, but do not inject it into a process you are not debugging or modding.
- Add-ons that load through the legacy `AddonInit(api, table)` path never needed this - the API table is handed to them instead of looked up.

---

## License

MIT, see [LICENSE](LICENSE).
