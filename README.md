# Taskbar OLED Indicator

A small experimental Windows 11 shell patch intended to remove the thin, always-visible auto-hide taskbar reveal line on OLED displays without replacing the taskbar or running a separate tray application.

## Goal

When the native Windows taskbar is set to auto-hide:
- Hide only the visual taskbar contents while the taskbar is in its hidden position.
- Keep the native taskbar window and edge hit-testing area intact.
- Preserve normal reveal when the pointer reaches the bottom edge.
- Load the code inside Explorer through the Windows Application Compatibility infrastructure; no standalone background utility, service, scheduled task or Run-key startup app.

## Status: experimental engineering prototype

**This is not a ready-to-install or validated release.** It uses undocumented Windows taskbar implementation details. An incompatible patch can crash Explorer or leave the taskbar in an incorrect visual state. Do not install it on a machine without a recovery path.

The first target is Windows 11 x64 build 26300.9711, as reported by the test PC. That is a test target, not a verified compatibility claim. A build-number check alone does not establish that taskbar private symbols and object layouts match.

## Design

The target technique is to obtain the primary taskbar's existing XAML root and set the root content opacity to zero while the taskbar is mostly outside the monitor, then restore opacity when it is revealed. The taskbar HWND is not hidden, resized, or repositioned by the patch. This should preserve Windows' native pointer-edge reveal region.

The intended architecture is an in-process Explorer shim loaded through the Windows Application Compatibility infrastructure. It is not a standalone app. An in-process implementation can still create a worker thread and hooks inside Explorer; it must not depend on a separate resident process.

## Handoff findings: failed custom-shim experiment (2026-11-03)

This section records what was actually tested and what the next engineer/AI agent should investigate. **Do not treat any of the former feature-branch installer packages as working releases.**

### User/test environment

Reported target PC:
- Windows 11 x64, OS build/UBR reported as **26300.9711**.
- `explorer.exe` version-resource values returned by PowerShell:
  - `FileDescription`: `Windows Explorer`
  - `ProductName`: `Microsoft® Windows® Operating System`
  - `FileVersion`: `10.0.26100.8117 (WinBuild.160101.0800)`
  - `OriginalFilename`: `EXPLORER.EXE.MUI`

The Explorer file version does not match the reported OS build. Re-check the actual OS build, Explorer image, taskbar.dll image and matching PDB on the target before relying on private symbols or hard-coded offsets.

### What was tested

A feature branch added:
- An x64 DLL named `TaskbarOLED.dll`.
- A generated AppCompat `.sdb` database intended to match `explorer.exe` and request a custom shim named `TaskbarOLEDIndicator`.
- PowerShell install/uninstall scripts using `sdbinst.exe`.
- A GitHub Actions package workflow.

Several CI runs succeeded. They proved only that the DLL compiled, expected export names existed, the SDB generator created a file that `SdbOpenDatabase` could reopen, and the PowerShell scripts parsed. They **did not** validate SDB semantics, Explorer matching, shim DLL resolution, DLL loading, or taskbar behaviour.

On the test PC:
- `sdbinst.exe` reported successful registration. The installed database appeared under `HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\InstalledSDB`, with description `Taskbar OLED Indicator` and a copied database path under `%WINDIR%\\AppPatch\\CustomSDB\\{GUID}.sdb`.
- After each package installation, `tasklist.exe /m TaskbarOLED.dll` reported: `No tasks are running which match the specified criteria.` There is no evidence that Explorer or any other process loaded the DLL.
- The user briefly observed the line absent on the desktop but visible in browser fullscreen. Because the DLL was not observed loaded, do not attribute that visual behaviour to this shim.
- Uninstallation eventually succeeded. Final checks showed the DLL absent from `AppPatch\\AppPatch64`, the Program Files installation directory absent, no matching InstalledSDB entry, and the cached custom SDB absent. `System32` was not modified.

### Most important unresolved issue: shim DLL loading contract

The custom DLL loading method used by the experiment is not validated.

- The [CustomShim project README](https://github.com/Fleex255/CustomShim#installing-custom-shim-modules) says its custom shim module must be placed in `%WINDIR%\\System32` (or `SysWOW64` for 32-bit) and renamed to one of the shim DLL names Windows accepts. It specifically suggests `AcRes.dll` as a suitable name. The project says this behaviour was tested on Windows 10 2004 and is unsupported by Microsoft.
- The [DLL Injection via Shim gist](https://gist.github.com/w4kfu/95a87764db7029e03f09d78f7273c4f4) says Windows 10 added an `SdbIsKnownShimDll` check in `SdbGetDllPath` and lists accepted names, but its sample later references `moo.dll` and copies it into `%WINDIR%\\AppPatch\\AppPatch64`. This is an apparent conflict between the sample and the allowlist note; the gist alone is not proof that arbitrary custom DLL names load on current Windows 11.
- The experiment used the project-specific name `TaskbarOLED.dll` in `%WINDIR%\\AppPatch\\AppPatch64`. Repeated `tasklist /m` checks showed no load. The next agent must resolve the loader path/name rules on the exact Windows build before making another installer. Do not assume that successful `sdbinst` registration means that the DLL can be found or loaded.
- Do not overwrite an existing Windows DLL. First inspect the exact target path, its owner/version/hash, and the loader's resolution behaviour. Prefer a VM or disposable test executable for the first loader proof, not Explorer.

### SDB lessons and prior mistakes

The [CustomShim binary SDB example](https://github.com/Fleex255/CustomShim/blob/master/sdb.sprint) is the relevant reference for the low-level `apphelp.dll` writer API. Its binary structure is:
- `DATABASE`
  - `LIBRARY`
    - `SHIM`
      - `INEXCLUDE`
  - `EXE`
    - `MATCHING_FILE`
    - `SHIM_REF`

In that binary example, `PRODUCT_NAME` is written inside `MATCHING_FILE`, not as a direct child of `EXE`. The experiment initially used the wrong product string (`Windows Explorer`, which is the FileDescription rather than the reported ProductName), then moved ProductName to the EXE level during a later attempted fix. The final tested SDB therefore still did not follow the reference hierarchy for that attribute. Verify the exact SDB tag placement and matching semantics with a known-good control database.

The shim name in the SDB and the name accepted by `GetHookAPIs` must match exactly. An earlier version had `TaskbarOLEDIndicator` in the SDB but checked for `TaskbarOLED` in the DLL; this mismatch was corrected in a later feature-branch build, but the DLL still did not load.

A successful `SdbOpenDatabase` call only shows the file can be opened. It is not a schema validator and does not prove that Windows matches the EXE or resolves the shim DLL.

### Diagnostics

The [CustomShim README logging notes](https://github.com/Fleex255/CustomShim#getting-shim-logs) describe:
- `SHIMENG_DEBUG_LEVEL=4` for shim-engine diagnostics.
- `LogFlags=255` (DWORD) under `HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags` for the 64-bit engine.
- `SHIM_DEBUG_LEVEL=4` and `SHIM_FILE_LOG` for logging emitted by the custom shim itself.

During this experiment, `LogFlags` and `SHIMENG_DEBUG_LEVEL` were set after Explorer was already running, so Explorer did not inherit the changed environment. No `AslLog_*_explorer.exe_*.txt` logs were found. Those diagnostic registry/environment changes were subsequently restored; final PowerShell checks returned blank values for both settings. A diagnostic run must start a fresh Explorer process with the environment established first (sign-out/sign-in is the reliable way to refresh the user environment). Keep logging temporary and restore prior values, not assumed defaults.

### Current code is not runtime-validated

The shell logic in `src/oled_shim.cpp` relies on undocumented taskbar internals:
- Private symbols from `taskbar.dll` and a Microsoft symbol-server lookup.
- Taskbar object/vtable discovery, private method calls and a layout offset inferred from a function prologue.
- A thread-specific Windows hook and XAML/WinRT calls to change the taskbar root opacity.
- A worker thread inside Explorer.

All of those steps remain unverified on the target OS build. A CI build cannot establish that private symbols exist, object layouts are correct, the hook can be installed, or Explorer remains stable.

### Recommended next steps for another agent

1. **Start from a minimal loader test.** Build a tiny x64 shim that only logs entry into `DllMain`, `GetHookAPIs` and `NotifyShims`. Do not include taskbar symbol lookup or XAML code yet.
2. **Prove the loader with a disposable test executable or VM first.** Establish the accepted DLL filename, exact search path and SDB format on Windows 11 build 26300.9711. Reconcile the CustomShim README's System32/allowlisted-name instructions with the gist's custom-name/AppPatch64 sample. Do not guess and do not overwrite any existing Windows DLL.
3. **Use the reference binary SDB hierarchy.** Put matching-file metadata in the correct `MATCHING_FILE` node, use the actual Explorer ProductName only if it is needed, and keep the SDB shim name identical to the name handled by `GetHookAPIs`.
4. **Collect shim-engine logs before testing Explorer.** Configure `SHIMENG_DEBUG_LEVEL` and `LogFlags` before starting the target process; use `SHIM_DEBUG_LEVEL`/`SHIM_FILE_LOG` for messages from the custom DLL.
5. **Only after a minimal DLL is proven to load**, test Explorer integration with a reversible rollback. First verify the module is actually loaded, then validate each symbol/object/XAML assumption separately.
6. **Keep the original product requirements.** The finished result should execute inside Explorer, preserve native taskbar edge reveal, require no separate app/service/tray process, and have a tested recovery/uninstaller path.

## Safety

- No install script should overwrite an existing system DLL.
- Install and uninstall must be reversible.
- The installer must refuse unknown Windows builds by default.
- Missing or mismatched symbols must disable the feature rather than guess at memory layouts.
- No prebuilt DLL should be distributed until the source compiles and has been tested on the target build.
- Always keep a way to launch Task Manager and remove the compatibility database in Safe Mode.

## Build and release

Build requirements: Visual Studio 2022 Desktop C++ workload, Windows 11 SDK, CMake, and PowerShell 7 (for development scripts).

The CI workflow should build the x64 DLL and compatibility database and publish them as an artifact. A successful compilation is not proof of runtime compatibility.

## Installation

There is no validated installer. **Do not install artifacts from the failed feature-branch experiment.**

The custom-shim loading references conflict on the DLL path and accepted filename (see [Handoff findings](#most-important-unresolved-issue-shim-dll-loading-contract)). The next implementation must establish this contract on a disposable Windows test target before packaging. Never overwrite an existing Windows DLL; inspect the target path, owner, version and hash, and provide a tested rollback before any system-wide placement.

## Uninstallation and recovery

The failed feature-branch experiment has been removed from the test PC. The last cleanup check confirmed that the project DLL, Program Files directory, registered database and cached SDB were absent; no System32 file was changed.

If a future test install is attempted:
1. Keep Task Manager available so an elevated PowerShell can be launched if Explorer becomes unstable.
2. Unregister the exact test SDB with `sdbinst.exe -u <database-path>`.
3. Stop/restart Explorer or sign out so the old process unloads any in-process DLL.
4. Remove only the exact project DLL after confirming its hash and that no process has it loaded.
5. Verify the corresponding InstalledSDB entry and cached SDB are gone.

## References and attribution

- [Taskbar Auto-Hide Motion](https://github.com/jinSeong-P/windhawk-floating-dock) — XAML-root discovery and taskbar implementation research (GPL-3.0).
- [CustomShim](https://github.com/Fleex255/CustomShim) — custom Application Compatibility shim sample, binary SDB example, loading restrictions and diagnostic logging guidance.
- [DLL Injection via Shim](https://gist.github.com/w4kfu/95a87764db7029e03f09d78f7273c4f4) — SDB/DLL loading example; contains an apparent conflict between its allowlist note and custom AppPatch64 example.
- [Microsoft: Application Compatibility Database](https://learn.microsoft.com/en-us/windows/win32/devnotes/application-compatibility-database).

Any code derived from GPL-licensed project code must preserve the applicable GPL terms and attribution. This project is not affiliated with Microsoft.
