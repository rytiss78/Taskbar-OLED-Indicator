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

The first target is Windows 11 x64 build 26300.9711, as reported by the intended test PC. That is a test target, not a verified compatibility claim. A build-number check alone does not establish that taskbar private symbols and object layouts match.

## Design

The target technique is to obtain the primary taskbar's existing XAML root and set the root content opacity to zero while the taskbar is mostly outside the monitor, then restore opacity when it is revealed. The taskbar HWND is not hidden, resized, or repositioned by the patch. This should preserve Windows' native pointer-edge reveal region.

The project uses an Application Compatibility database to load a custom shim DLL into explorer.exe. The DLL is not a standalone process; its code executes in Explorer. It will still consume a small amount of Explorer address space and may perform a one-time symbol lookup.

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

A release installer will be documented here after the custom shim database and DLL are validated. **Do not manually copy any DLL into System32 or register an experimental SDB from an unverified build.**

## Uninstallation and recovery

The final package must include an elevated PowerShell uninstaller that first unregisters the Application Compatibility database, then restarts Explorer so the DLL is unloaded, and finally removes only files created by this project.

If Explorer fails after a test install:
1. Press Ctrl+Shift+Esc to open Task Manager.
2. Use Run new task and start an elevated PowerShell.
3. Run the project's uninstaller with the Explorer restart option.
4. If normal sign-in is unavailable, use Safe Mode or Windows Recovery Environment to unregister the installed SDB with sdbinst.exe -u.

## References and attribution

- [Taskbar Auto-Hide Motion](https://github.com/jinSeong-P/windhawk-floating-dock) — XAML-root discovery and taskbar implementation research (GPL-3.0).
- [CustomShim](https://github.com/Fleex255/CustomShim) — custom Application Compatibility shim sample and SDB notes.
- [Microsoft: Application Compatibility Database](https://learn.microsoft.com/en-us/windows/win32/devnotes/application-compatibility-database).

Any code derived from GPL-licensed project code must preserve the applicable GPL terms and attribution. This project is not affiliated with Microsoft.