I was really upset with microsoft putting the windows system troubleshooters only online especially network troubleshooting tools ... which is pretty retarded to put online 
to be clear that is no longer running locally on your computer when it has no internet access.. so the internet troubleshooting tool is only avaialable on the internet
utterly retarded.

so I wanted to make an alternative windows troubleshooter
that will do what the old troubleshooters did and help fix issues that for whatever reason windows has decided not to let its endusers to to keep their computer working.... its absurd imho.

Download the zip and run from there. 
https://github.com/WilliamAshley2019/WinDiagPro/archive/refs/heads/main.zip

  This version adds a graphical interface under topology as I aim to work towards the original trouble shoooter for networking issues fix.

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

## Quick start

If you want the easiest way to try the current build, download the zip and
run it from there:

https://github.com/WilliamAshley2019/WinDiagPro/archive/refs/heads/main.zip

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
    ├── TopologyView.h/.cpp   # self-drawn (GDI) network topology diagram control
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
- **Topology** — an auto-refreshing visual diagram of every adapter and how
  it routes (or doesn't) to the internet - see "Multi-NIC topology diagnosis"
  below.
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

## Multi-NIC topology diagnosis and the Topology tab

This grew out of a real multi-NIC troubleshooting case: one adapter with a
working internet connection, a second adapter used for a direct cable link
to another PC, and that second adapter intermittently losing its DHCP lease
and ending up with a link-local (APIPA, `169.254.x.x`) address while still
carrying a stale routable default gateway - an invalid combination
(RFC 3927 says `169.254.0.0/16` must never carry a usable gateway) that
causes exactly the "some things load, some don't" symptom that's miserable
to chase through `ipconfig` output alone.

**Adapters are now classified by role**, not just judged by "does it have an
IP":

- **Routed** - normal IP + working default gateway
- **Gateway scope mismatch** - the invalid pattern above; flagged as a `Fail`
  with a specific explanation and a specific fix (see below), not lumped in
  with generic "network is broken"
- **Direct link** - link-local address, no gateway attached (the *correct*
  behavior for a direct cable to another PC with no DHCP server)
- **Isolated** - has an IP but no gateway at all
- **Disconnected** - cable unplugged / Wi-Fi off
- **Virtual** - Hyper-V/WSL/VPN switches, detected by description and excluded
  from "is the network actually down" logic so they can't cause a false alarm

Each adapter's diagnostic entry now also reports its **route metric** (which
adapter Windows prefers for the default route) and the **gateway's Neighbor
Unreachability Detection state** (Reachable/Probe/Stale/Unreachable/etc, via
`GetIpNetTable2`) - "Probe" specifically means Windows has a candidate route
to that gateway but hasn't confirmed it actually works, which is exactly the
signal that mattered in the case above.

**New targeted fix:** right-clicking a failing adapter row now offers
"Release & renew DHCP on this adapter" (`RepairEngine::ReleaseRenewAdapter`)
- cycling *one* adapter's lease, not every adapter on the machine. That's
the actual fix for the scenario above; a global Winsock/TCP-IP reset would
have been overkill and would have disturbed the adapter that was already
working fine.

**New Topology tab** - a self-drawn diagram (This PC → each adapter →
Internet), colour-coded by the roles above, auto-refreshing every 3 seconds,
click any box for its full details in a panel underneath. This is the
"draw wires between boxes instead of reading text tables" view - built as a
native GDI control (`TopologyView.h/.cpp`) rather than a chat-embedded
visualization, since it needs to live inside the compiled app itself. It's
intentionally **read-only / auto-laid-out for this first version**, not an
interactive drag-and-drop network designer - see "Ideas for next steps"
below for that distinction.

**New safety-net repair actions**, matching the "snapshot before you touch
anything, make it reversible" discipline that a good manual troubleshooting
session already follows:
- **Backup Network Drivers** - exports every installed network adapter's
  driver package (already in the local driver store - nothing downloaded)
  via the built-in `pnputil` tool, so a driver is available to reinstall
  offline if an aggressive repair ever leaves an adapter without one.
- **Backup Current Network Configuration** - snapshots `ipconfig /all`,
  `route print`, and `netsh interface ip show config` to a timestamped file,
  purely as a before/after reference.

### What I deliberately did not build: raw DHCP DISCOVER/OFFER probing

One idea from that troubleshooting session was sending a raw DHCP DISCOVER
and watching for an OFFER, to know for certain whether a DHCP server is
responding at all (the `dhcptest`-style approach). I looked at this
seriously and decided against it for now: Windows' own DHCP Client service
normally owns UDP port 68 system-wide, so a clean implementation needs
either raw IP sockets (which some antivirus products flag as suspicious
behavior - awkward for a troubleshooting tool to trigger) or temporarily
stopping the DHCP Client service mid-test (invasive on a multi-NIC machine
where another adapter may depend on it). I also can't validate real
broadcast/DHCP runtime behavior in my own sandbox the way I can validate a
compile. Given all that, the honest move was to build the safer
alternative that's still genuinely useful (NUD gateway state + role
classification above) and flag this one as a considered, deliberate
deferral rather than a fragile half-implementation. Happy to build it
carefully as a dedicated follow-up if you still want it, most likely via
the "stop DHCP Client service, test, restart" path with very clear warnings
around it.



## Repair actions inspired by (not copied from) other open-source tools

I looked at what's out there in the open-source Windows repair/troubleshooter
space to find ideas worth pulling in. Short version: almost everything
actively maintained and well-regarded turned out to be **MIT**, not GPLv3 —
[Windows Maintenance Tool](https://github.com/ios12checker/Windows-Maintenance-Tool),
[ChrisTitusTech/winutil](https://github.com/ChrisTitusTech/winutil), and
[Sophia Script for Windows](https://github.com/farag2/Sophia-Script-for-Windows)
are all MIT. The GPLv3 ones I found were
[RWU](https://matbanik.info/hobbies/systems/posts/reset-windows-update-guide/)
(a modern Windows Update reset tool) and
[NETworkManager](https://github.com/BornToBeRoot/NETworkManager) (a C#
network toolkit — ping/traceroute/port-scan/subnet-calc — good idea source
for expanding the Network tab later, though not directly portable into this
C++ codebase). Worth flagging: **Dism++ is not actually open source** — only
its translation/rules files are; the application itself is closed-source per
the maintainers' own admission in their GitHub issues.

Since WinDiagPro is native C++ with no runtime dependencies, the practical
integration path is reimplementing the *technique* natively rather than
pulling in someone else's C#/Java/PowerShell/batch code — that keeps the
single-EXE, no-install, license-simple design intact. First one added this
way:

- **Full Windows Update Reset** (Repair tab, and offered directly when a
  right-click on a failed "Windows Update" service row shows it) — stops
  `wuauserv`/`BITS`/`CryptSvc`/`msiserver`, renames
  `%windir%\SoftwareDistribution` and `%windir%\System32\catroot2` out of the
  way (nothing is deleted — old folders get a timestamped `.bak` suffix, so
  it's fully reversible, matching the "reviewable, reversible" philosophy RWU
  itself advocates), then restarts the services. This is the standard fix for
  a Windows Update service that's running but never actually installs
  anything — the plain service restart already in the Repair tab usually
  isn't enough for that specific failure mode.


## Ideas for next steps (not yet built)

- An **interactive** version of the Topology tab - drag adapters/gateway
  boxes, draw/edit connections by hand, and compare "what Windows is
  actually doing" against "what you want it to do" side by side. The
  current Topology tab is deliberately read-only/auto-laid-out; a real
  editor is a substantially bigger UI undertaking (hit-testing for wire
  endpoints, an undo stack, validating edits against what Windows will
  actually accept) and worth scoping as its own project phase.
- Raw DHCP DISCOVER/OFFER probing - see the dedicated section above on why
  this was deliberately deferred rather than rushed.
- CLI flags for `.md`/`.json` export (`/md`, `/json`).
- Wire the Repair tab's "requires reboot" actions to offer an immediate
  restart prompt.
- A small offline knowledge-base file (JSON/INI shipped alongside the EXE)
  so the rules engine's diagnoses — and the QuickActions mapping — can be
  edited/extended without a rebuild.
- A portable/no-install build profile (static CRT) for running straight off
  a USB stick in WinRE, where the VC++ redistributable may not be present.
  Happy to add `<MultiThreaded>` (static) runtime linking if you want that —
  just say the word.

