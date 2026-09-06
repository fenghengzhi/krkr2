# KrKr2 Web

基于 WebAssembly 的 **KiriKiri2 引擎**（T Visual Presenter）移植，让吉里吉里引擎游戏直接在浏览器中运行。

> 这是一个专注于 Web 平台的个人分支，不接受 Pull Request。

**语言 / Language**: 中文 | [English](README.md)

---

## 支持浏览器

Chrome、Edge、Firefox、Safari（任何支持 WebAssembly + SharedArrayBuffer 的现代浏览器）。

---

## 编译

### 依赖工具

- [Emscripten SDK (emsdk)](https://emscripten.org/docs/getting_started/downloads.html) **6.0.9**（与 CI 固定版本一致）
- [vcpkg](https://learn.microsoft.com/en-us/vcpkg/get_started/get-started)
- [ninja](https://github.com/ninja-build/ninja/releases)
- [cmake 3.31.1+](https://cmake.org/download/)
- `bison 3.8.2+`
- `python3`

### 环境变量

```bash
export VCPKG_ROOT=/path/to/vcpkg
/path/to/emsdk/emsdk install 6.0.9
/path/to/emsdk/emsdk activate 6.0.9
source /path/to/emsdk/emsdk_env.sh   # 自动设置 EMSDK
```

### 编译步骤

切换 SDK 版本后，应使用新的构建目录并重新编译所有依赖。Emscripten 不保证跨版本 ABI 兼容。

配置前先预编译端口库（vcpkg 构建也会使用这些头文件）：

```bash
embuilder build libpng libpng-mt libpng-legacysjlj libpng-mt-legacysjlj
embuilder build freetype freetype-legacysjlj
embuilder build harfbuzz harfbuzz-mt
embuilder build sdl2 sdl2-mt
embuilder build sdl2_ttf sdl2_ttf-mt
```

Web 构建使用 JSPI，支持 Release 和 Debug。Debug 使用 `Web Debug Config` preset，输出目录为 `out/web/debug`。

```bash
cmake --preset "Web Release Config"
cmake --build out/web/release
```

### 产物位置

```
out/web/release/
  index.html
  index.js
  index.wasm
  vlfs.js
  assets.zip
```

`index.worker.js` 等额外文件取决于 SDK 版本和构建选项。

---

## 运行

Web 版本需要[跨域隔离](https://web.dev/cross-origin-isolation-guide/)响应头（`COOP` / `COEP`）以支持 `SharedArrayBuffer`。普通 HTTP 服务器无法正常运行。

使用项目自带的 `coi-server.py`：

```bash
python3 coi-server.py out/web/release [http端口] [https端口] [选项]
```

服务器同时启动：
- **HTTP** 端口 8080（默认）— 用于 `localhost` 本地调试
- **HTTPS** 端口 8443（默认）— 用于局域网内其他设备访问

然后在浏览器中打开 `http://localhost:8080/index.html`。

### 直接指定游戏文件

#### 单个 .xp3 文件

通过 `--xp3` 参数让服务器托管本地 `.xp3` 文件：

```bash
python3 coi-server.py out/web/release --xp3 /path/to/game/data.xp3
```

服务器会将该文件映射到 `/data.xp3` 路径，并输出包含 `?xp3=` 参数的完整 URL。打开该 URL 后，网页会自动下载并启动游戏，无需手动选择文件。

#### ZIP 压缩包

通过 `--zip` 参数托管包含游戏文件的 `.zip` 压缩包。网页会在浏览器中解压并加载游戏：

```bash
python3 coi-server.py out/web/release --zip /path/to/game.zip
```

如果压缩包中包含多个 `.xp3` 文件，会弹出选择对话框。使用 `--entry` 自动选择其中一个：

```bash
python3 coi-server.py out/web/release --zip /path/to/game.zip --entry data.xp3
```

#### URL 参数

也可以直接通过 URL 查询参数指定游戏来源：

| 参数 | 说明 |
|------|------|
| `?xp3=<url>` | 从指定 URL 加载单个 `.xp3` 文件 |
| `?game=<url>` | 从指定 URL 下载并解压 `.zip` 压缩包 |
| `?entry=<name>` | 当压缩包中包含多个 `.xp3` 时，自动选择指定的文件（配合 `game` 使用） |

### 局域网 HTTPS 访问

`SharedArrayBuffer` 要求 `localhost` 或 HTTPS 环境。如需局域网访问，将 `server.crt` 和 `server.key` 放在 `coi-server.py` 同级目录：

```bash
openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt -days 365 -nodes
```

---

## TODO

- [ ] 等 iOS Safari 支持 [JSPI（JavaScript Promise Integration）](https://github.com/aspect-build/aspect-cli/issues/1)后，使用 JSPI + `-fwasm-exceptions` 替代 Asyncify + `NO_DISABLE_EXCEPTION_CATCHING`。这将消除基于 `invoke_*` 的异常处理，显著减小 wasm 体积（~26MB → ~17MB），降低每个 Worker 的内存开销。

---

## 支持的游戏列表

见 [games list](./doc/support_games.txt)。

---

## 许可证

MIT 许可证。详见 [LICENSE](./LICENSE)。
