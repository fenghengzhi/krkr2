# KrKr2 Web

A WebAssembly port of the **KiriKiri2 engine** (T Visual Presenter), allowing KiriKiri engine games to run directly in modern browsers.

> This is a personal fork focused exclusively on the Web platform. Pull requests are not accepted.

**语言 / Language**: [中文](README_CN.md) | English

---

## Supported Browsers

Chrome, Edge, Firefox, Safari (any browser with WebAssembly + SharedArrayBuffer support).

---

## Build

### Prerequisites

- [Emscripten SDK (emsdk)](https://emscripten.org/docs/getting_started/downloads.html) **6.0.9** (pinned in CI)
- [vcpkg](https://learn.microsoft.com/en-us/vcpkg/get_started/get-started)
- [ninja](https://github.com/ninja-build/ninja/releases)
- [cmake 3.31.1+](https://cmake.org/download/)
- `bison 3.8.2+`
- `python3`

### Environment Variables

```bash
export VCPKG_ROOT=/path/to/vcpkg
/path/to/emsdk/emsdk install 6.0.9
/path/to/emsdk/emsdk activate 6.0.9
source /path/to/emsdk/emsdk_env.sh   # sets EMSDK automatically
```

### Build Steps

After changing SDK versions, use a fresh build directory and rebuild all dependencies. Emscripten does not guarantee ABI compatibility across versions.

Prewarm the ports before configuring (vcpkg also compiles against these headers):

```bash
embuilder build libpng libpng-mt libpng-legacysjlj libpng-mt-legacysjlj
embuilder build freetype freetype-legacysjlj
embuilder build harfbuzz harfbuzz-mt
embuilder build sdl2 sdl2-mt
embuilder build sdl2_ttf sdl2_ttf-mt
```

Both Release and Debug are supported; the Web build uses JSPI. For Debug, use the `Web Debug Config` preset and `out/web/debug`.

```bash
cmake --preset "Web Release Config"
cmake --build out/web/release
```

### Output Files

```
out/web/release/
  index.html
  index.js
  index.wasm
  vlfs.js
  assets.zip
  manifest.webmanifest
  sw.js
  pwa/
    icon-192.png
    icon-512.png
```

Additional files such as `index.worker.js` depend on the SDK and build options.

The build also copies **PWA** assets (`manifest.webmanifest`, `sw.js`, `pwa/*.png`) next to `index.html`. After serving over `localhost` or HTTPS with COOP/COEP (e.g. `coi-server.py`), Chromium-based browsers can offer **Install app**; the service worker uses network-only fetch so engine files are not stale-cached.

---

## Running

The Web build requires [Cross-Origin Isolation](https://web.dev/cross-origin-isolation-guide/) headers (`COOP` / `COEP`) for `SharedArrayBuffer`. A regular HTTP server will not work.

Use the included `coi-server.py`:

```bash
python3 coi-server.py out/web/release [http_port] [https_port] [options]
```

The server starts:
- **HTTP** on port 8080 (default) — for `localhost` debugging
- **HTTPS** on port 8443 (default) — for LAN access from other devices

Then open `http://localhost:8080/index.html` in your browser.

### Serving a Game File Directly

#### Single .xp3 file

Use `--xp3` to have the server host a local `.xp3` file:

```bash
python3 coi-server.py out/web/release --xp3 /path/to/game/data.xp3
```

The file is served at `/data.xp3`, and the printed URL includes the `?xp3=` query parameter. Opening that URL will automatically download and start the game without manual file selection.

#### ZIP archive

Use `--zip` to serve a `.zip` archive containing the game files. The web page will extract it in-browser and load the game:

```bash
python3 coi-server.py out/web/release --zip /path/to/game.zip
```

If the archive contains multiple `.xp3` files, a selection dialog will appear. Use `--entry` to auto-select one:

```bash
python3 coi-server.py out/web/release --zip /path/to/game.zip --entry data.xp3
```

#### URL parameters

You can also pass game sources via URL query parameters directly:

| Parameter | Description |
|-----------|-------------|
| `?xp3=<url>` | Load a single `.xp3` file from the given URL |
| `?game=<url>` | Download and extract a `.zip` archive from the given URL |
| `?entry=<name>` | Auto-select this `.xp3` when the archive contains multiple (use with `game`) |

### HTTPS for LAN Access

`SharedArrayBuffer` requires `localhost` or HTTPS. To enable HTTPS for LAN access, place `server.crt` and `server.key` alongside `coi-server.py`:

```bash
openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt -days 365 -nodes
```

---

## TODO

- [ ] Switch from Asyncify + `NO_DISABLE_EXCEPTION_CATCHING` to JSPI + `-fwasm-exceptions` once iOS Safari supports [JSPI (JavaScript Promise Integration)](https://github.com/aspect-build/aspect-cli/issues/1). This will eliminate `invoke_*`-based exception handling, significantly reduce wasm binary size (~26MB → ~17MB), and lower per-Worker memory overhead.

---

## Supported Games

See [games list](./doc/support_games.txt).

---

## License

MIT License. See [LICENSE](./LICENSE) for details.
