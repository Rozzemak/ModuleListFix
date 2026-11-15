# ModuleListFix

Restores `K32EnumProcessModules` inside a process that hides its own module list, so
components that locate a DLL by enumerating the process work again.

## Symptom

A game or application runs fine, its mods partially work, but anything that looks itself up
by module enumeration fails. The clearest example is ReShade: effects compile and render, but
every add-on refuses to load:

    Loading add-on from '...\some-addon.addon64' ...
    ERROR | Failed to load add-on from '...\some-addon.addon64' with error code 1114!

1114 is `ERROR_DLL_INIT_FAILED`, i.e. the add-on's `DllMain` returned FALSE. A
measured example of what is really happening inside such a process (diagnostic build that
logs the API result):

    before patch: K32EnumProcessModules ok=0 needed=0 err=6      <- ERROR_INVALID_HANDLE
    after patch:  K32EnumProcessModules ok=1 count=2048

Zero modules. Modern ReShade add-ons ship `reshade.hpp`, whose
`internal::get_reshade_module_handle()` finds the ReShade core by enumerating every
module and taking the one that exports `ReShadeRegisterAddon` and
`ReShadeUnregisterAddon`. With an empty enumeration that lookup returns null, the
add-on's registration helper returns false, `DllMain` returns FALSE, and the loader
reports 1114. Nothing is wrong with the add-on, its ReShade API version, or its bitness.

Other components fail the same way, and are useful as corroboration:

* UE4SS: `Fatal Error: Was unable to enumerate game modules. Error: [0x6] The handle is invalid.`
* add-ons that fall back to the legacy `AddonInit(api, table)` path keep working,
  because that path receives the API table instead of looking it up.

## Why the process is in that state

Wrappers that need to be invisible to integrity checks (manual mapping, protectors, some
DRM/anti-debug layers, hypervisor-assisted loaders) unlink or corrupt their own entries in the
PEB loader lists. The process still runs, but the loader data no longer looks like a normal
module list, and kernel32's enumeration validates it and gives up with
`ERROR_INVALID_HANDLE`.

Walking the PEB list directly still works in those processes, which is what this DLL does.

## What it does

1. Walks `PEB->Ldr->InLoadOrderModuleList` to get the module list.
2. Replaces `K32EnumProcessModules` in **kernel32**, **kernelbase** and the psapi aliases
   with that walk. Patching only the kernel32 forwarder is not enough: the loader resolves
   imports straight to the implementation in kernelbase, so a caller whose import table is
   already bound never reaches the kernel32 copy.
3. Clamps the result to the caller's buffer and returns success. Callers commonly pass a fixed
   8 KB array (1024 handles); reporting "you need more" is what an untouched implementation
   does, and those callers treat that as failure.
4. Keeps a watchdog thread that re-applies any patch a protection layer reverts.

Nothing else is hooked, and no memory outside those few entry points is touched.

## Usage

Load it before the component that needs the enumeration.

ReShade, through the existing add-on-from-DllMain hook (ReShade.ini):

    [ADDON]
    LoadFromDllMain=ModuleListFix.dll

Any other host: `LoadLibrary` it early from a proxy DLL you already control, before the
dependent component initialises.

The DLL writes `ModuleListFix.log` next to itself: the PEB walk result, every patched site,
and one line per intercepted call, so you can confirm the dependent component actually reaches
it.

Expected result on the ReShade case:

    Registered add-on "..." using ReShade API version 18.

## Build

MSVC, x64. From an "x64 Native Tools Command Prompt for VS":

    build.bat

or directly:

    cl /nologo /LD /EHsc /O2 /Fe:ModuleListFix.dll src\ModuleListFix.cpp /link /MACHINE:X64

`/LD` builds the DLL; the file name does not matter, load it by whatever name
your host expects.

## Caveats

* User mode only. If the loader data itself is gone rather than just inconsistent, there is
  nothing to walk.
* Some protection layers also protect their own code pages against writes. If the watchdog
  log fills with `re-patched site` lines, the patch is being reverted faster than it is
  applied and the process is not a good candidate.
* Hooking a system DLL is invasive by nature. It is safe here because the replacement returns
  the same shape of data the original would, but do not ship it into a process you are not
  debugging or modding.

## License

MIT, see LICENSE.
