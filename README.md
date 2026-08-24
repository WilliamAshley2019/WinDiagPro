I was really upset with microsoft putting the windows system troubleshooters only online especially network troubleshooting tools ... which is pretty retarded to put online to be clear that is no longer running locally on your computer when it has no internet access.. so the internet troubleshooting tool is only avaialable on the internet utterly retarded.

so I wanted to make an alternative windows troubleshooter that will do what the old troubleshooters did and help fix issues that for whatever reason windows has decided not to let its endusers to to keep their computer working.... its absurd imho.

Download the zip and run from there. https://github.com/WilliamAshley2019/WinDiagPro/archive/refs/heads/main.zip
Be sure to download and extract the entire package as the application may not execute as a standalone exe.
I will potentially be adding some probing capabilities that may trigger windows due to possible malicious usages of some of the functions that I may use for diagnostic purpses particularly dhcp probing, I need to test this, and it may show up on your system based on your system setting - this is expected behavior if it does occur as I'm not 100% sure how much diagnostic information and packet access via windows ports can be done without creating suspicion on windows internal system ports being used maliciously. There is nothing malicious with it, the plan is to sniff and listen to all dhcp traffic on the network and compare that to the actual settings then attempt to validate the traffic to determine if there are malicious or disrupting dhcp activities on the network. The plan was to leave these tools on the intranet side of the network, rather than usage to resolve the internet side.  These comparison tools may be able to validate and provide more points of reference that may offset or determine issues that exist due to one of the data resolvers being corrupted - such that secondary system may show the mismatch that wouldn't be detected if only one tool was used that was compramised or corrupted. The last resort is to bundle third party tools that are already signed to run those functions.
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

**Nothing else needs to be installed, to build or to run this.** To build:
Visual Studio 2026 (or 2022) with the "Desktop development with C++"
workload — that's the Windows SDK, which is all this project uses. No
NuGet packages, no Windows App SDK, no third-party libraries. To run: just
the compiled EXE — it links the static C++ runtime, so it doesn't even need
the Visual C++ Redistributable present. Everything it shells out to
(`ipconfig`, `netsh`, `sfc`, `DISM`, `pnputil`, `w32tm`, Notepad, Device
Manager, Event Viewer, Control Panel applets) already ships with Windows.
The GitHub projects named later in this file (in "Repair actions inspired
by other open-source tools") are research references only — ideas that got
reimplemented natively in this codebase, never pulled in as dependencies.

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
    ├── HelpContent.h         # curated offline troubleshooting reference, baked into the EXE
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
  Windows Defender service status, hosts file review, and browser/WinHTTP
  proxy configuration.
- **Topology** — an auto-refreshing visual diagram of every adapter and how
  it routes (or doesn't) to the internet - see "Multi-NIC topology diagnosis"
  below.
- **Repair** — checkbox list of repair actions (flush DNS, reset Winsock,
  reset TCP/IP, renew DHCP, restart DHCP/DNS/NLA/Firewall services); check
  the ones you want and click **Run Checked Repairs**. A log of what
  happened appears below, and the dashboard re-scans automatically after.
- **Help** — a curated offline troubleshooting reference compiled directly
  into the EXE - available with zero network access. See "Keeping the
  machine diagnosable" below.
- **Report** — full report, with **Save as .txt / .html / .md / .json**
  buttons (saves to Documents by default). `.md` and `.json` exist
  specifically so you can hand the report to an AI assistant or paste it
  into a ticket system in a format that parses cleanly, rather than only the
  human-oriented `.txt`/`.html` forms. Every save now asks "open it now?" -
  `.txt`/`.md`/`.json` open in Notepad specifically (some systems have no
  default handler for `.md`/`.json` at all, which would otherwise prompt an
  unhelpful "how do you want to open this?" dialog; Notepad is also exactly
  what you want if you're about to copy/paste into an editor, browser, or AI
  chat), while `.html` opens in your default browser. **Open Last Saved
  Report** and **Open Containing Folder** buttons let you get back to it
  later without re-saving.

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

**Critical bug fix: gateway detection was silently unreliable.** Adapter
gateway detection relied solely on `GetAdaptersAddresses`' `FirstGatewayAddress`
field, which is a documented-in-practice unreliable Windows API quirk - it
can come back empty for IPv4 gateways learned via plain DHCP even though the
gateway is fully configured and working (confirmed via a real report: two
adapters both showed `ipconfig`-visible working gateways, but WinDiagPro
reported both as `FAIL - no default gateway configured`, cascading into a
false "No active network connection" root-cause diagnosis on a perfectly
healthy network). `ipconfig.exe` avoids this by not relying solely on that
field - it effectively cross-references the actual IP routing table. Gateway
detection now does the same: `GetIpForwardTable2` (the same API already used
for the Active Route Selection and Manually-Configured Routes checks below)
is the primary, authoritative source, with the adapter field kept only as a
fallback if that somehow comes up empty too. This fix is at the data source,
so every downstream consumer - adapter role classification, the Topology
tab's colors, the rules-engine diagnosis - self-corrects with it.

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

**Bug fix: selection used to revert after ~3 seconds.** The auto-refresh
timer was unconditionally clearing which box was selected every time it
rebuilt the diagram, so a clicked box's details would flash and then revert
to the generic "click any box" hint - nowhere near enough time to read or
copy anything. Selection is now tracked by the clicked node's identity
(its title) rather than its position in the list, and re-resolved against
the freshly-rebuilt diagram on every refresh - so the details you clicked
for now stay on screen until you deliberately click something else.

**What the gateway actually is, and its real limits.** The gateway shown
for each adapter comes from `GetAdaptersAddresses` - it's exactly what
Windows itself has configured (via DHCP or a static setting), nothing more.
Concretely, this means: switches and hubs between you and that gateway are
completely invisible (they're Layer 2 - no IP tool on any OS can see them);
the gateway IP could be a router, an AP in router mode, or a modem, with no
way to tell which from the LAN side alone; and carrier-grade NAT (CGNAT) -
common on cable/mobile ISPs - happens entirely at the ISP's edge and is
invisible to `ipconfig` by definition.

**New: Trace Route to Internet** (Network tab button) actually sees past
your own gateway. It shells out to `tracert` (walking the real path hop by
hop toward a public IP, `-d` for speed and `-S` to force a specific source
adapter when needed) and flags any hop that falls in `100.64.0.0/10` -
that block is reserved (RFC 6598) exclusively for CGNAT, so seeing it on
the path is about as close to definitive proof as you can get without
asking your ISP directly. This is on-demand only (can take up to a minute)
and, unlike the automatic checks, deliberately sends real packets toward
the public internet - if the internet is down, it's still informative:
you'll see exactly which hop the path stops at.

**New: two checks that confirm rather than infer your own machine's
routing.** The metric shown per adapter tells you which one *should* win
for internet traffic, but that's an inference - two direct checks now
confirm it instead:
- **Active route selection** calls `GetBestInterfaceEx` to ask Windows
  directly which adapter it will actually use for general internet traffic
  right now, rather than assuming the lowest metric wins.
- **Manually-configured routes** scans the IPv4 routing table
  (`GetIpForwardTable2`) for any route whose `Origin` is `NlroManual` -
  i.e. not auto-generated by DHCP/router-advertisement/normal config - since
  a VPN client or leftover static route can silently override normal
  adapter selection for a specific destination, in a way the metric column
  alone won't reveal.

**New: two "what's actually out there" checks**, collecting more of what
Windows already knows rather than leaving it locked in `ipconfig`/`arp`
output you'd have to go look up separately:
- **Local network devices** reads the ARP/neighbor table
  (`GetIpNetTable2`) and lists every device Windows has actually resolved a
  MAC address for on your LAN, with which adapter saw it and whether it's
  flagged as a router - a live inventory of what's actually reachable
  on-link, not just your own configuration.
- **DNS client cache** shows how many entries are currently cached (via
  `ipconfig /displaydns`) *before* you decide to flush it - seeing what's
  cached is more precise diagnostically than "flush it and hope."

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


## Keeping the machine diagnosable and fixable with the internet down

This is the actual point of the project, so it's worth calling out
specifically what's aimed at it in this round:

- **Static CRT linking.** The build now links `MultiThreaded`/
  `MultiThreadedDebug` instead of the DLL runtime, so the compiled EXE does
  not depend on the Visual C++ Redistributable being installed. That matters
  a lot for a WinRE/recovery-USB scenario or a freshly-reimaged machine -
  there's no internet to fetch the redistributable if it's missing, so the
  tool now simply doesn't need it.
- **Hosts file check** (Security tab) - flags any active (non-comment,
  non-default-localhost) entry, since a hijacked or stale hosts file is a
  classic, easy-to-miss cause of "some sites won't resolve while others
  work fine." Right-click to open it directly in Notepad for review.
- **Proxy checks** (Security tab) - both the WinINet proxy (browsers/most
  apps) and the separate WinHTTP proxy (Windows Update and many background
  services use this one specifically) are checked independently, since a
  leftover corporate/VPN proxy in just one of the two is a very common,
  confusing cause of "my browser works but Windows Update doesn't" or vice
  versa. New **Reset WinHTTP Proxy** repair action to clear it.
- **System clock / time sync check** (System tab) - a wrong system clock
  breaks HTTPS/TLS certificate validation, which looks exactly like "the
  internet is down" for every secure site, while having nothing to do with
  networking at all. This is one of the most commonly missed causes because
  of how unrelated it looks.
- **The Help tab** - curated, original troubleshooting reference text
  (`HelpContent.h`) compiled directly into the executable: how to read the
  Topology tab's colors, a triage order for "internet doesn't work" that
  rules out categories before you touch anything invasive, the
  clock/HTTPS gotcha above, a walkthrough for a stuck Windows Update, and
  the "snapshot before you change anything" discipline the Repair tab's
  backup actions support. This is available with the network fully down and
  no access to anyone's support servers - which is the entire premise of
  replacing Microsoft's now-online-only troubleshooters.
- **System Restore point check** (System tab) - reports whether any restore
  points exist and when the most recent one was taken, with a **Create
  Restore Point Now** repair action (via PowerShell's `Checkpoint-Computer`).
  This is a whole-system undo button - registry, drivers, system files, not
  just network settings - and it works completely offline. Worth creating
  one before any invasive repair.
- **Autostart entries check** (Security tab) - lists registry Run/RunOnce
  entries (HKLM and HKCU). This directly complements the hosts-file and
  proxy checks: a common reason a network fix "comes back" after a restart
  is malware or a stray installer reapplying its hosts/proxy/DNS changes via
  an autostart entry every boot. Right-click to jump to Task Manager's
  Startup tab.
- **Elevated Command Prompt / PowerShell buttons** (Help tab) - a safety
  valve so a full terminal is always one click away, without needing to
  find it through a Start menu that might itself be misbehaving. Since
  WinDiagPro already runs elevated, the launched terminal inherits that
  elevation automatically.

## On the large tool/API reference material

A lot of reference material has come up across this project - CLI tool
lists, enterprise routing protocols, and low-level kernel/hardware APIs.
Worth being explicit about the triage, since most of it is either already
covered, or a deliberate no:

- **Standard CLI tools** (`ping`, `tracert`, `route print`, `arp -a`,
  `nslookup`, `netsh`, `ipconfig`, driver/service queries) - this is what
  WinDiagPro already builds on throughout; the additions this round (local
  device inventory, DNS cache) are exactly filling in gaps from that list.
  `netstat`/active-connections remains a good, still-open addition (see
  below).
- **Enterprise routing protocols** (BGP, OSPF, EIGRP, HSRP/VRRP, spanning
  tree, LACP) - these configure *routers and switches*, not a Windows
  client. Out of scope for what this tool is (a client-side troubleshooter),
  not a client-side networking gap.
- **Raw sockets, kernel-mode packet interception (WFP/NDIS callouts), and
  DPDK-style kernel bypass** - deliberately not going here. Setting this
  aside for the same reason the raw DHCP DISCOVER/OFFER probe was declined
  earlier: this class of capability (packet spoofing, promiscuous capture,
  custom kernel drivers) is what security tooling looks like, not
  troubleshooting tooling, and is exactly the kind of behavior that gets a
  program flagged by antivirus - a bad trade for a tool whose whole premise
  is being trustworthy when things are already going wrong. It's also
  usually the wrong tool for the actual problem: none of the scenarios this
  project has dealt with (APIPA+gateway mismatch, CGNAT, DHCP lease issues,
  proxy/hosts misconfiguration) needed anything below the standard IP Helper
  API layer to diagnose or fix.
- **CPU-level intrinsics and line-rate performance tuning** (SIMD CRC32,
  cache-line flushing, thread affinity pinning, nanosecond timing) - this is
  datacenter/HFT-grade infrastructure tooling for diagnosing multi-gigabit
  throughput bottlenecks. Not applicable to what this tool does; a home or
  small-office machine's networking problems are never bottlenecked at the
  CPU-cache level.

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
- A per-adapter "temporarily switch DNS to a public resolver (1.1.1.1 /
  8.8.8.8)" repair action, for the "one DNS server is down but the network
  is otherwise fine" case the Help tab talks about.
- An "Active Connections" view (`netstat`-based) - what's actually connected
  right now, useful for spotting something unexpected without needing the
  internet to look it up.
- Querying your router directly for its WAN-side IP via UPnP/IGD (SSDP
  discovery + the `GetExternalIPAddress` SOAP call) - a fully local,
  no-internet-needed way to confirm CGNAT definitively rather than infer it
  from a traceroute hop, on routers that expose it. More involved and
  router-firmware-dependent (many disable UPnP by default) than the
  traceroute approach already built, so scoped as a later addition.
- Saving the Topology diagram itself as an image file, for a printable/
  offline-shareable reference of a specific fault condition.
- Per-adapter MTU reporting (via `GetIpInterfaceEntry`'s `NlMtu` field,
  already the same API pattern used for the metric field) - a mismatched
  MTU is a real, if less common, cause of specific-site failures over
  VPN/PPPoE connections that the current checks don't surface.
