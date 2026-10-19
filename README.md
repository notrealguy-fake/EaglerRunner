# EaglerRunner

[![Windows build](https://github.com/notrealguy-fake/EaglerRunner/actions/workflows/windows.yml/badge.svg)](https://github.com/notrealguy-fake/EaglerRunner/actions/workflows/windows.yml)

A tiny, fast C++ shell for Windows that plays Eaglercraft-style HTML clients straight from
your disk. It wraps the WebView2 engine, gives the loaded page **unrestricted WebSocket, TCP
and UDP networking**, and keeps a **persistent client library** so a saved client keeps
working even after you delete the original `.html` file.

No browser tabs, no Electron, no oversized runtime — one small `.exe` plus a `resources`
folder.

## Download

Grab the newest build from the [**Releases**](https://github.com/notrealguy-fake/EaglerRunner/releases) page:

| File | What it is |
|---|---|
| `EaglerRunner_setup.exe` | Installer (Start menu + optional desktop shortcut, uninstaller) |
| `EaglerRunner-portable.zip` | Portable: unzip anywhere, run `EaglerRunner.exe` |

Requires Windows 10 1809+ and the (free, preinstalled on Windows 11) **Microsoft Edge
WebView2 Runtime**. No VC++ redistributable needed — the build uses a static runtime.

---

## Features

| | |
|---|---|
| **Launcher first** | Opens on a FoldCraft-style home screen — no file-picker popup every time |
| **In-app downloader** | Fetches Eaglercraft builds, unpacks them and files them into your library automatically |
| **My Clients library** | Persistent copies of your clients; play them even after deleting the original files |
| **Recent files** | Quick re-open list, with "missing" markers |
| **TCP + UDP for pages** | `window.eaglerNet` API — raw sockets, IPv4/IPv6, nothing proxied or blocked |
| **WebSocket passthrough** | Runs in the WebView2 engine exactly like a browser (ws:// and wss://) |
| **Two themes** | Dark and light launcher artwork, switchable from the launcher or Settings |
| **Potato PC Mode** | Software rendering + single renderer process for very weak machines |
| **Light** | WebView2 is already on Windows, so the app itself is a few hundred KB |
| **Quality of life** | Drag & drop a `.html` file, version picker, toasts, fullscreen, zoom, dev tools |

---

## Requirements

* Windows 10 1809+ (WebView2 Runtime, already installed on Windows 11 and most Windows 10 PCs)
* To build: **Visual Studio 2022 Build Tools** with the *Desktop development with C++* workload

---

## Building

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

That script finds Visual Studio, downloads the WebView2 SDK into `third_party/webview2`
if needed, configures CMake, and builds:

```
build\Release\EaglerRunner.exe
build\Release\resources\launcher.html
```

Manual build, if you prefer:

```powershell
third_party\fetch_webview2.ps1                 # once
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

The build uses a **static C runtime** (`/MT`) by default, so the executable needs no
VC++ redistributable. Configure with `-DEAGLER_SHARED_CRT=ON` if you want `/MD`.

---

## Using it

1. Run `EaglerRunner.exe` — the launcher home screen appears.
2. Press **START** (bottom right) and pick an Eaglercraft client `.html` file, or open the
   built-in downloader (the download icon in the header, or `F6`) to fetch a version
   straight into your library.
3. The client loads and plays. Point it at whatever server you want; WebSocket traffic is
   passed straight through.
4. To keep a one-off file forever: **File → Save to Launcher** (or `Ctrl+S`), type a name. A
   copy is stored under `%LOCALAPPDATA%\EaglerRunner\library\<name>\`, along with any sibling
   files that share the client's base name (for example `client.html` + `client.epk` +
   `client_files\`).
5. Delete the original file if you like — the library copy still runs, and shows up in the
   launcher's **My Clients** list.

When a file is opened, EaglerRunner also offers to save it to the library automatically
(toggle that prompt in **Settings → Ask to save**). The version picker in the bottom bar
remembers your choice; launching a specific version directly from it lands in a future
update.

### Shortcuts

| Key | Action |
|---|---|
| `Ctrl+O` | Open HTML file |
| `Ctrl+S` | Save current file to the launcher library |
| `Ctrl+L` | Back to the launcher |
| `F6` | Open the Eaglercraft downloader |
| `Ctrl+R` / `F5` | Reload the page |
| `F11` / `Esc` | Toggle fullscreen |
| `F12` | Developer tools |
| `Ctrl` `+` / `-` / `0` | Zoom in / out / reset |

### Where things live

```
%LOCALAPPDATA%\EaglerRunner\
├── settings.ini          # options, window geometry, recent files
├── library\<name>\       # saved clients (manifest.txt describes each one)
└── WebView2\             # browser profile (cookies/cache for the loaded pages)
```

---

## Networking

Nothing is filtered, proxied or rate limited. The page can reach any host and port,
including `localhost`, and both IPv4 and IPv6 work.

* **WebSocket** — handled natively by the engine: `new WebSocket("ws://...")` works as in a
  browser, including plain `ws://` (the app serves pages from a local virtual host rather
  than `file://`, so mixed-content rules do not block game traffic).
* **Raw TCP/UDP** — extra API injected into every loaded page (including Eaglercraft
  clients, if they want to use it):

```js
// TCP
const id = eaglerNet.tcp.connect("play.example.com", 25565);
eaglerNet.tcp.send(id, new Uint8Array([...]));   // Uint8Array / ArrayBuffer / string
eaglerNet.tcp.close(id);

// UDP
const sock = eaglerNet.udp.open(0);              // 0 = any local port
eaglerNet.udp.send(sock, "1.2.3.4", 19132, data);
eaglerNet.udp.close(sock);

// Events
eaglerNet.on("open",  e => console.log("connected", e.id));
eaglerNet.on("data",  e => console.log(e.type, e.id, e.host, e.port, e.data)); // e.data = Uint8Array
eaglerNet.on("close", e => console.log("closed", e.id));
eaglerNet.on("error", e => console.warn(e.id, e.message));
```

Socket events for TCP: `open`, `data`, `close`, `error`. For UDP: `open` (reports the bound
port), `data` (includes sender `host`/`port`), `close`, `error`.

The launcher page can also drive the application:

```js
eaglerRunner.openFileDialog();
eaglerRunner.openFile("C:\\path\\client.html");
eaglerRunner.saveCurrentToLibrary();
eaglerRunner.openLibraryEntry("My Client");
eaglerRunner.setLowEnd(true);
eaglerRunner.reload();
eaglerRunner.fullscreen();
eaglerRunner.exit();
```

---

## Look and feel

The launcher ships with two hand-graded photographic backgrounds:

| File | Theme | Grade |
|---|---|---|
| `resources/bg-dark.png` | Dark (default) | Lantern-lit night village — teal shadows, amber highlights, bloom on the lanterns, deep night sky |
| `resources/bg-light.png` | Light | Cherry blossoms over water — sunlit warmth, richer pinks, dehazed sky |

They were produced from `background_dark.jpg` and `background_light.webp` by
`tools/grade_backgrounds.py` (denoise → clarity → split toning → lift/gamma/gain → S-curve →
vibrance → bloom → vignette → unsharp mask). Re-run it any time to regenerate the PNGs:

```powershell
python tools\grade_backgrounds.py
```

Switch themes with the **Dark / Light** buttons in Settings (or the sun icon in the header);
the choice is saved in `settings.ini` and restored on the next launch. The app icon comes
from `src/app.ico`, built by `tools/make_icon.py`; the in-app logo is
`resources/logo.svg` (grass block + play badge). `src/app.rc` embeds the icon, version
info and a DPI-aware manifest into the exe.

---

## Testing the networking

Two helpers are included to prove that TCP/UDP work end to end:

1. Start the echo servers (TCP `25599`, UDP `25598`):

```powershell
powershell -ExecutionPolicy Bypass -File tools\echoserver.ps1 -Seconds 120
```

2. Launch the app straight into the self-test page:

```powershell
build\Release\EaglerRunner.exe tools\nettest.html
```

The page connects over TCP, echoes a payload, then opens a UDP socket and echoes a
datagram. The result is written to the page (and to the window title as
`NETTEST PASS tcp=1 udp=1`, or `FAIL` with the failing side), so it can be checked without
looking at the screen:

```powershell
(Get-Process EaglerRunner).MainWindowTitle
```

---

## Project layout

```
CMakeLists.txt              build configuration (static CRT, LTCG, /O2)
build.ps1                   one-shot build script
src/main.cpp                Win32 window, WebView2 host, menus, library dialogs
src/net_bridge.h/.cpp       TCP/UDP worker thread (select-based, non-blocking)
src/downloader.h/.cpp       fetches + unpacks Eaglercraft builds from the CDN
src/library.h/.cpp          persistent "My Clients" library
src/settings.h/.cpp         settings.ini persistence
src/shim.h                  JavaScript injected into every page
src/util.h/.cpp             strings, base64, percent-encoding, files
resources/launcher.html     the launcher GUI (home screen, downloads, settings)
resources/logo.svg          app logo
installer/eaglerrunner.iss  Inno Setup installer script
.github/workflows/windows.yml   CI: build + portable zip + installer + releases
tools/echoserver.ps1        TCP/UDP echo servers for testing
tools/nettest.html          networking self-test page
tools/capture_window.ps1    screenshot the running app (dev helper)
tools/check_js.js           syntax-checks the embedded JavaScript (dev helper)
third_party/fetch_webview2.ps1   downloads the WebView2 SDK
```

---

## In-app downloader

**Header download icon / `F6` → Get Eaglercraft.** The list is served from a manifest of
official Eaglercraft builds (JS and WASM-GC editions of 1.8.8, 1.12.2, 1.20.5 and the
legacy 1.5.2). Choosing one:

1. downloads the archive to `%LOCALAPPDATA%\EaglerRunner\downloads\`,
2. unpacks it with the `tar.exe` that ships with Windows 10 1803+,
3. copies the unpacked client into your library under its display name,
4. cleans the temporary files up and turns the button into **Play**.

If unpacking ever fails, the archive is kept and reported so nothing is lost.

---

## Roadmap

* Launch the version selected in the bottom-left picker directly (the wiring is stubbed today).
* macOS port (separate WKWebView host; the Win32/WebView2 build is Windows-only).
* Custom background images and more launcher themes.

---

## Performance notes

WebView2 runs the page out-of-process in the installed Edge engine, so rendering cost is
close to a normal browser and the shell itself is nearly free. To keep old machines happy:

* `--disable-backgrounding-occluded-windows`, `--disable-background-timer-throttling` and
  `--disable-renderer-backgrounding` keep the client running at full speed when the window
  loses focus (Minecraft-like clients otherwise stutter or freeze).
* **Potato PC Mode** adds `--disable-gpu`, software compositing and a single renderer
  process; the launcher page also drops its animations.
* Release builds use `/O2 /GL /LTCG` and a static runtime.

## Troubleshooting

* **"Could not start the web engine (WebView2)"** — install the free *Microsoft Edge
  WebView2 Runtime* and relaunch.
* **A saved client says "file missing"** — the library folder was moved or deleted; re-save
  it from the original HTML.
* **Client refuses to connect to a server** — that is the client's/server's own rule, not
  the wrapper: nothing here blocks connections. Check the server address, `ws://` vs `wss://`
  and whether the server allows the client version.

---

## License

[MIT](LICENSE) © 2026 notrealguy-fake
