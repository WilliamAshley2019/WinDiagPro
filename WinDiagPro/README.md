# WinDiagPro

An offline-first Windows troubleshooter/diagnostic tool, built as a native
Win32 desktop app in C++. It exists because Microsoft has been moving parts
of its built-in troubleshooters (network diagnostics, some driver checks,
etc.) toward online-dependent flows, which doesn't help on a machine that
has no internet access — that's exactly the case this tool targets.

**Everything in this build is local.** Adapter/DHCP/DNS/gateway checks talk
only to your own LAN and to devices you're already configured to use (your
router, your configured DNS servers). WMI, the event log, service control,
SFC, and DISM are all local OS APIs. There is no telemetry, no cloud
reporting, and no dependency that needs to be downloaded at build or run
time — it's a single self-contained EXE.

## What changed from the original draft

The original draft (WinDiag / WinDiagApp) mixed in three things that don't
fit an offline standalone tool, and I removed them:

- **WinUI 3 / XAML GUI** — requires the Windows App SDK runtime and a
  packaging/MSIX story to distribute. Replaced with a plain Win32 GUI
  (TabControl + ListViews + native controls) that compiles with nothing
  but the Windows SDK that ships with Visual Studio, and runs as one EXE
  with no extra runtime to install.
- **WinPcap-based packet capture** — WinPcap is unmaintained, isn't
  preinstalled, and isn't offered here; it would have been dead code without
  it. Left out entirely rather than shipping a non-functional feature.
- **"Cloud Reporter" (HTTPS report upload)** — directly contradicts the
  offline goal, so it's gone. Reports are saved as local `.txt`/`.html`
  files instead (see the Report tab).

Everything else — network checks, system checks, the repair engine, and the
reporting — was rewritten from scratch against a single consistent data
model (the original had two incompatible `WinDiag`/`DiagnosticsEngine`
class designs from two different drafts). New: WMI-based hardware/problem-
device checks, Windows Event Log queries, a real rules/diagnosis engine
that reasons about *combinations* of failures instead of one flat if/else
chain per symptom, and a CLI mode for scripted/offline-recovery use.

## Project layout

```
WinDiagPro/
├── WinDiagPro.sln
├── WinDiagPro.vcxproj / .vcxproj.filters
├── WinDiagPro.rc / resource.h
├── README.md
└── src/
    ├── main.cpp              # wWinMain: launches GUI, or CLI mode with /cli
    ├── Common.h/.cpp         # shared types, string/process/clipboard helpers
    ├── DiagnosticsEngine.h/.cpp  # network/system/hardware/security checks
    ├── WMIHelper.h/.cpp      # thin local-only WMI (COM) query wrapper
    ├── RepairEngine.h/.cpp   # flush DNS, reset winsock/TCP-IP, renew DHCP, restart services
    ├── RulesEngine.h/.cpp    # combines check results into ranked root-cause diagnoses
    ├── QuickActions.h/.cpp   # maps a failing check to a concrete fix (repair action or launched tool)
    ├── ReportGenerator.h/.cpp# builds + saves .txt / .html / .md / .json reports
    ├── MainWindow.h/.cpp     # native Win32 GUI (TabControl + ListViews, copy + context menus)
    └── resource.h
```

## Manifest handling (and why there's no app.manifest file)

Two different manifest-embedding approaches both failed on a real VS2026
build before landing on the current one:

- **v1.0** shipped a separate `app.manifest`, merged in via
  `<AdditionalManifestFiles>` (the standard approach). This hit
  `LNK1327: failure during running mt.exe` — a known source of flaky
  failures in that merge step on some machines/AV setups.
- **v1.0.1** tried embedding `app.manifest` directly as an `RT_MANIFEST`
  resource in `WinDiagPro.rc` with `<GenerateManifest>false</GenerateManifest>`.
  On a real VS2026 build that property did not stop the linker from *also*
  generating its own default manifest resource, so the two collided:
  `CVTRES : fatal error CVT1100: duplicate resource. type:MANIFEST, name:1`.

**v1.0.2 (current)** drops the external manifest file entirely and lets the
linker synthesize the whole manifest itself, which is the one code path that
doesn't involve either mt.exe or a hand-authored resource competing with the
linker's own:

- UAC elevation → `<UACExecutionLevel>RequireAdministrator</UACExecutionLevel>`
  and `<UACUIAccess>false</UACUIAccess>` in `WinDiagPro.vcxproj` (native
  linker properties, no XML manifest needed).
- Common Controls v6 (themed ListView/TabControl) → a
  `#pragma comment(linker, "/manifestdependency:...")` at the top of
  `main.cpp` — the standard technique for this exact dependency.
- Per-monitor DPI awareness → `SetProcessDpiAwarenessContext()` called at the
  very start of `wWinMain` (loaded dynamically via `GetProcAddress`, so it
  simply no-ops on any Windows version that doesn't have it, rather than
  becoming a hard dependency).

Net effect: identical runtime behavior (elevated, themed controls, DPI
aware), zero external manifest file, zero merge step, nothing for mt.exe or
CVTRES to collide over.

## Building (Visual Studio 2026, Windows 11 25H4)

1. Open `WinDiagPro.sln` in Visual Studio 2026.
2. If prompted to retarget the platform toolset/SDK, accept it — the
   project targets `v143`/Windows SDK `10.0` (latest installed), which
   should auto-map to whatever ships with VS2026. This project uses only
   Windows SDK headers/libs and the C++17 standard library — no NuGet
   packages, no Windows App SDK, nothing else to install.
3. Select `Release|x64` (or `Debug|x64` while iterating) and Build.
4. The EXE lands in `bin\x64\Release\WinDiagPro.exe`.
5. Run it elevated ("Run as administrator") — the manifest requests this
   automatically, and most repair actions (service restarts, Winsock/TCP-IP
   reset, SFC/DISM) need it. Windows will show the standard UAC prompt.

### Validation note

I don't have a Windows/MSVC environment available where I generated this,
so I cross-compiled every source file with MinGW-w64 (a real, independent
C/C++ toolchain targeting Windows) as a stand-in check: all eight source
files compile cleanly with zero warnings under `-Wall -Wextra`, and the
whole project links into a real PE32+ Windows executable with no missing
symbols. That's a genuinely useful check for *code* bugs (typos, wrong
signatures, missing includes/libs, unresolved references) — but it's a
different linker from a different toolchain, so it can't catch MSVC-specific
linker/manifest-tool behavior. That's exactly what happened here: the
`LNK1327` and then `CVT1100` failures were both in that category, and only
showed up on a real VS2026 build. If VS2026 reports anything else, paste the
error text back and I'll fix it immediately.

## Using it

**GUI mode** (default — just run the EXE): a full diagnostic runs
automatically on launch. Tabs:

- **Dashboard** — top failing/warning items across every category, plus a
  plain-English "likely root cause" narrative from the rules engine.
- **Network** — adapters, gateway/DNS reachability, Winsock catalog health,
  an informational (non-alarming) internet-reachability probe.
- **System** — critical services, system-drive free space, recent System
  event log errors, plus on-demand SFC and DISM CheckHealth/ScanHealth
  (these are slow — minutes — so they're not run automatically).
- **Hardware** — WMI query for any device Windows currently flags with a
  Device Manager error code.
- **Security** — Windows Firewall profile status (Domain/Private/Public),
  Windows Defender service status.
- **Repair** — checkbox list of repair actions (flush DNS, reset Winsock,
  reset TCP/IP, renew DHCP, restart DHCP/DNS/NLA/Firewall services); check
  the ones you want and click **Run Checked Repairs**. A log of what
  happened appears below, and the dashboard re-scans automatically after.
- **Report** — full report, with **Save as .txt / .html / .md / .json**
  buttons (saves to Documents by default). `.md` and `.json` exist
  specifically so you can hand the report to an AI assistant or paste it
  into a ticket system in a format that parses cleanly, rather than only the
  human-oriented `.txt`/`.html` forms.

### Copying and acting on individual results

- **Any text box** (the Dashboard diagnosis, the Report tab, the Repair log)
  is a standard read-only text field — click-drag or Ctrl+A to select, Ctrl+C
  or right-click → Copy, same as any other Windows text field. Nothing extra
  to learn there.
- **Every list (Dashboard/Network/System/Hardware/Security/Repair)** supports
  multi-select (Ctrl/Shift+click, like File Explorer) and Ctrl+C copies the
  selected row(s) as tab-separated text — paste straight into Notepad,
  Notepad++, a browser text box, or a spreadsheet column-aligned.
- **Right-click any row** for a context menu with **Copy Selected Row(s)** /
  **Copy All Rows**, plus — where one applies — a **fix-it action** specific
  to that row. For example, right-clicking a low-disk-space warning offers
  "Open Storage Settings" and "Open Disk Cleanup"; a failed service offers
  "Restart the service"; a disconnected adapter offers "Open Network
  Connections"; a DNS/gateway failure offers "Flush DNS" / "Reset Winsock";
  a Device Manager problem offers "Open Device Manager"; a disabled firewall
  profile offers "Open Windows Firewall settings". These call the same
  RepairEngine actions as the Repair tab, or launch the actual Windows
  tool/settings page that addresses it — the recommendation text now comes
  with a button, not just advice. See `QuickActions.cpp` for the full
  mapping if you want to add more.

**CLI mode** (for scripting, or running from a WinRE/recovery USB where you
just want a report without clicking through a GUI):

```
WinDiagPro.exe /cli /full                 Run every check, print + save a report
WinDiagPro.exe /cli /network              Network checks only
WinDiagPro.exe /cli /system               System checks only
WinDiagPro.exe /cli /hardware             Hardware (WMI) checks only
WinDiagPro.exe /cli /security             Security checks only
WinDiagPro.exe /cli /full /report:D:\Logs Save the report to a specific folder
WinDiagPro.exe /cli /full /html           Save as .html instead of .txt
```
(CLI mode currently always saves `.txt`, or `.html` with `/html` — `.md`/`.json`
export is GUI-only for now; shout if you want CLI flags for those too.)

## Ideas for next steps (not yet built)

- CLI flags for `.md`/`.json` export (`/md`, `/json`).
- Wire the Repair tab's "requires reboot" actions to offer an immediate
  restart prompt.
- A small offline knowledge-base file (JSON/INI shipped alongside the EXE)
  so the rules engine's diagnoses — and the QuickActions mapping — can be
  edited/extended without a rebuild.
- Per-adapter repair targeting (currently DHCP renew is "all adapters").
- A portable/no-install build profile (static CRT) for running straight off
  a USB stick in WinRE, where the VC++ redistributable may not be present.
  Happy to add `<MultiThreaded>` (static) runtime linking if you want that —
  just say the word.
