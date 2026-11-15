# ModuleListFix

**ReShade add-ons (and other mod loaders) fail with error `1114` in a process that hides its own module list. This DLL puts the module list back.**

A small Windows x64 DLL you load before ReShade. No configuration, no per-game patching, no rebuild of the add-ons themselves: the add-ons you already have start loading.

---

## The failure

ReShade itself works: the hooks install, the swap chain is created, effects compile and render. Only add-ons die, every time, with the same three symptoms:

```log
Loading add-on from 'D:\Games\SomeGame\renodx-ue-extended.addon64' ...
ERROR | Failed to load add-on from 'D:\Games\SomeGame\renodx-ue-extended.addon64' with error code 1114!

Loading add-on from 'D:\Games\SomeGame\renodx-dlss5.addon64' ...
ERROR | Failed to load add-on from 'D:\Games\SomeGame\renodx-dlss5.addon64' with error code 1114!

Loading add-on module "D:\Games\SomeGame\renodx-dlss.addon64" ...
ERROR | Failed to load add-on from 'D:\Games\SomeGame\renodx-dlss.addon64' with error code 126!
```

What each code actually means here:

| Code | Windows meaning | What it is telling you |
|---|---|---|
| `1114` | `ERROR_DLL_INIT_FAILED` | `LoadLibrary` reached the add-on, its `DllMain` ran, and `DllMain` returned `FALSE`. Nothing in the file is corrupt. |
| `126` | `ERROR_MOD_NOT_FOUND` | The file named in `[ADDON] LoadFromDllMain` no longer exists (usually a leftover after renaming an add-on). Harmless noise, fix by editing the ini. |
| `193` | `ERROR_BAD_EXE_FORMAT` | 32/64-bit mismatch - the one cause people usually expect, and *not* this bug. |

The add-on's own log often agrees that it barely started:

```log
[session] v10.8.0 pid=14908 ends=logged exe=Game.exe
[exit] v10.8.0 pid=14908 up_ms=79 unload
[exit] v10.8.0 pid=14908 up_ms=94 unload
```

And unrelated in-process tooling fails the same way, which is the giveaway that the process, not the add-on, is broken:

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

Why is it dead? Wrappers that must be invisible to integrity checks - manual mapping, protectors, some anti-debug layers, hypervisor-assisted loaders - unlink or corrupt their own entries in the PEB loader lists. The process runs normally, but the loader data no longer validates, and kernel32's enumeration gives up. The same reason makes the main executable show up in crash reports as `faulting module: unknown, version 0.0.0.0`.

Walking the PEB list directly still works in those processes. That is the whole fix.

---

## What ModuleListFix does

1. Walks `PEB->Ldr->InLoadOrderModuleList` to build the real module list.
2. Replaces `K32EnumProcessModules` in **kernel32**, **kernelbase** and **psapi** (`EnumProcessModules`, `...Ex`) with that walk.
3. Clamps the result to the caller's buffer and returns success.
4. Runs a watchdog thread that re-applies any patch a protection layer reverts.

Two details that decide whether this works at all, both learned the hard way:

- **Patch kernelbase, not just kernel32.** The loader binds imports straight to the implementation inside `KernelBase.dll`, so an add-on whose import table is already resolved never reaches the kernel32 forwarder.
- **Clamp instead of reporting a short buffer.** Callers pass a fixed 8 KB array (1024 handles). Returning `FALSE` with `*needed = 16384` - which is what the real API does - is treated as failure by those callers. Fill what fits and return `TRUE`.

Nothing else is hooked.

---

## Usage

### ReShade

Load it before ReShade scans for add-ons, through the add-on-from-DllMain hook in `ReShade.ini`:

```ini
[ADDON]
LoadFromDllMain=ModuleListFix.dll
```

Put `ModuleListFix.dll` next to the game executable. Remove or comment the line if you ever need to disable it - a missing file only logs the harmless `126`.

### Any other host

`LoadLibrary` it early from a proxy DLL you already control, before the component that needs module enumeration initialises.

---

## Checking that it took

The DLL writes `ModuleListFix.log` beside itself:

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

Before the fix the same three add-ons produced `1114`, `1114`, `1114` on all four load attempts, every launch.

---

## Is this your bug?

You need this if all of the following are true:

- ReShade itself works (effects render, overlay opens) but **every** add-on fails with `1114`;
- the game ships unusual loader shims (proxies for system DLLs, a loader, an emulator layer) or runs under a hypervisor-level protection;
- other in-process tooling that enumerates modules also fails - `UE4SS`, trainers, overlays, script loaders.

You do **not** need it if the add-on fails with `193` (bitness), `126` (missing file or dependency), or if only one specific add-on fails while others load.

---

## Build

MSVC, x64. From an **x64 Native Tools Command Prompt for VS**:

```bat
build.bat
```

or:

```bat
cl /nologo /LD /EHsc /O2 /Fe:ModuleListFix.dll src\ModuleListFix.cpp /link /MACHINE:X64
```

The output file name does not matter; load it under whatever name your host expects.

---

## Caveats

- **User mode only.** If the loader data is gone rather than merely inconsistent, there is nothing to walk.
- **Some protections are aggressive.** If `ModuleListFix.log` fills with `watchdog: re-patched site`, the patch is being reverted faster than it is applied and that process is not a good candidate.
- **Hooking a system DLL is invasive by nature.** It is safe here because the replacement returns the same shape of data the original would, but do not inject it into a process you are not debugging or modding.
- ReShade add-ons that load through the legacy `AddonInit(api, table)` path never needed this - the API table is handed to them instead of looked up.

---

## License

MIT, see [LICENSE](LICENSE).
