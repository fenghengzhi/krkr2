#include "Platform.h"

#ifdef __EMSCRIPTEN__

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#undef st_atime
#undef st_mtime
#undef st_ctime
#include <sys/time.h>
#include <unistd.h>
#include <dirent.h>
#include <filesystem>
#include <algorithm>

#include <spdlog/spdlog.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>
#include <cmath>

#include "EventIntf.h"
#include "StorageIntf.h"
#include "StorageImpl.h"
#include "Defer.h"
#include "ui/MessageBox.h"
#include "cocos2d/MainScene.h"
#include "LayerFrameDumper.h"
#include "VirtualLazyFS.h"

void TVPGetMemoryInfo(TVPMemoryInfo &m) {
    size_t heapSize = (size_t)sbrk(0);
    m.MemTotal = heapSize / 1024;
    m.MemFree = (heapSize - (size_t)sbrk(0)) / 1024;
    m.SwapTotal = 0;
    m.SwapFree = 0;
    m.VirtualTotal = heapSize / 1024;
    m.VirtualUsed = 0;
}

#include <sched.h>
void TVPRelinquishCPU() { sched_yield(); }

bool TVP_utime(const char *name, time_t modtime) {
    timeval mt[2];
    mt[0].tv_sec = modtime;
    mt[0].tv_usec = 0;
    mt[1].tv_sec = modtime;
    mt[1].tv_usec = 0;
    return utimes(name, mt) == 0;
}

tjs_int TVPGetSystemFreeMemory() {
    return static_cast<tjs_int>((size_t)sbrk(0) / (1024 * 1024));
}

tjs_int TVPGetSelfUsedMemory() {
    return static_cast<tjs_int>((size_t)sbrk(0) / (1024 * 1024));
}

std::string TVPGetPackageVersionString() { return "web"; }

bool TVPCheckStartupPath(const std::string &path) { return true; }

void TVPControlAdDialog(int adType, int arg1, int arg2) {}
void TVPForceSwapBuffer() {}

std::string TVPGetCurrentLanguage() {
    char buf[16] = {0};
    EM_ASM({
        var lang = navigator.language || navigator.userLanguage || 'en_us';
        lang = lang.replace('-', '_').toLowerCase();
        stringToUTF8(lang, $0, 16);
    }, buf);
    std::string locale(buf);
    if (locale.empty()) return "en_us";
    return locale;
}

// ---------------------------------------------------------------------------
// 文件持久化：旧 fsafs_* host-stream/回写机制已被 VirtualLazyFS 吸收 ——
// 写路径经 VLFS overlay，关闭时由 vlfs.js 的 onWriteClose 钩子（shell.html
// 赋值）做 IndexedDB write-through + MEMFS 小文件镜像 + FSA 主机目录回写。
// ---------------------------------------------------------------------------

EM_JS(int, web_alert, (const char* msg, const char* title), {
    console.warn('[alert] ' + UTF8ToString(title) + '\n' + UTF8ToString(msg));
    return 0;
});

EM_JS(int, web_confirm, (const char* msg, const char* title), {
    return confirm(UTF8ToString(title) + '\n' + UTF8ToString(msg)) ? 0 : 1;
});

int TVPShowSimpleMessageBox(const ttstr &text, const ttstr &caption,
                            const std::vector<ttstr> &vecButtons) {
    auto msg = text.AsStdString();
    auto cap = caption.AsStdString();
    if (vecButtons.size() <= 1) {
        web_alert(msg.c_str(), cap.c_str());
        return 0;
    }
    if (vecButtons.size() == 2) {
        return web_confirm(msg.c_str(), cap.c_str());
    }
    std::vector<std::string> btnStrs;
    btnStrs.reserve(vecButtons.size());
    for (const auto &b : vecButtons) {
        btnStrs.push_back(b.AsStdString());
    }
    TVPMessageBoxForm::show(cap, msg, static_cast<int>(btnStrs.size()),
                            btnStrs.data(), [](int) {});
    return 0;
}

extern "C" int TVPShowSimpleMessageBox(const char *pszText,
                                       const char *pszTitle, unsigned int nButton,
                                       const char **btnText) {
    std::vector<ttstr> vecButtons{};
    for (unsigned int i = 0; i < nButton; ++i) {
        vecButtons.emplace_back(btnText[i]);
    }
    return TVPShowSimpleMessageBox(pszText, pszTitle, vecButtons);
}

int TVPShowSimpleInputBox(ttstr &text, const ttstr &caption,
                          const ttstr &prompt,
                          const std::vector<ttstr> &vecButtons) {
    spdlog::warn("web platform simple input box not fully implemented");
    return 0;
}

bool TVPCreateFolders(const ttstr &folder);

static bool _TVPCreateFolders(const ttstr &folder) {
    if (folder.IsEmpty())
        return true;

    if (TVPCheckExistentLocalFolder(folder))
        return true;

    const tjs_char *p = folder.c_str();
    tjs_int i = folder.GetLen() - 1;

    if (p[i] == TJS_W(':'))
        return true;

    while (i >= 0 && (p[i] == TJS_W('/') || p[i] == TJS_W('\\')))
        i--;

    if (i >= 0 && p[i] == TJS_W(':'))
        return true;

    for (; i >= 0; i--) {
        if (p[i] == TJS_W(':') || p[i] == TJS_W('/') || p[i] == TJS_W('\\'))
            break;
    }

    ttstr parent(p, i + 1);
    if (!TVPCreateFolders(parent))
        return false;

    // VLFS 命名空间下（如游戏目录内建 savedata/）目录只存在于 VLFS
    // registry，MEMFS 上父链缺失会让 create_directory 报错——VLFS mkdir
    // 自动补全父链；MEMFS 侧尽力同步（失败忽略）
    if (VLFS::Enabled()) {
        std::error_code ec;
        std::filesystem::create_directory(folder.AsStdString().c_str(), ec);
        return VLFS::MkDir(folder.AsStdString().c_str()) == 0;
    }

    return !std::filesystem::create_directory(folder.AsStdString().c_str());
}

bool TVPCreateFolders(const ttstr &folder) {
    if (folder.IsEmpty())
        return true;

    const tjs_char *p = folder.c_str();
    tjs_int i = folder.GetLen() - 1;

    if (p[i] == TJS_W(':'))
        return true;

    if (p[i] == TJS_W('/') || p[i] == TJS_W('\\'))
        i--;

    return _TVPCreateFolders(ttstr(p, i + 1));
}

bool TVP_stat(const char *name, tTVP_stat &s) {
    // VLFS（懒加载虚拟文件系统）优先：游戏文件不在 MEMFS 中
    if (VLFS::Enabled()) {
        uint64_t size = 0;
        int isDir = 0;
        if (VLFS::Stat(name, &size, &isDir)) {
            s.st_mode = isDir ? S_IFDIR : S_IFREG;
            s.st_size = size;
            s.st_atime = 0;
            s.st_mtime = 0;
            s.st_ctime = 0;
            return true;
        }
    }

    struct stat t;
    if (stat(name, &t) != 0) {
        return false;
    }

    s.st_mode = t.st_mode;
    s.st_size = t.st_size;
    s.st_atime = t.st_atim.tv_sec;
    s.st_mtime = t.st_mtim.tv_sec;
    s.st_ctime = t.st_ctim.tv_sec;

    return true;
}

bool TVP_stat(const tjs_char *name, tTVP_stat &s) {
    return TVP_stat(ttstr{name}.AsStdString().c_str(), s);
}

// ===== 平台边界（Web）：主线程 vsync 锁相 tick =====
// libkrkr2.so 的 TVPGetRoughTickCount32 @0xA2BF90 = steady_clock 毫秒
// (CLOCK_MONOTONIC)，主循环由 Choreographer 驱动：引擎主线程对该时钟的逐帧
// 采样时刻精确落在 vsync 栅格上（执行时刻对栅格抖动 ±0.2ms），因此脚本层
// 逐帧读 System.getTickCount 得到严格均匀的时间步。法娘 ActionManager 的
// `tick - lasttick >= interval` 整数毫秒门依赖该性质（13ms 重锚 vs 16ms 门，
// 裕量仅 0.67ms）。浏览器上 RAF 时间戳本身对 vsync 栅格抖动 ±1-2ms（实测
// 栅格残差 p95=1.17ms），回调执行时刻再叠加派发延迟抖动 ±0.6ms；主线程若
// 直接读 CLOCK_MONOTONIC，抖动会暴露给脚本，造成整数毫秒门偶发跳拍
// （实测 4.2% → 渐变 25ms 视觉跳步）与 timer 重锚相位漂移。Web 无法获得
// Android 级的回调时刻精度，故复刻其可观察行为：主线程 tick 在帧内冻结为
// "锁相后的帧栅格时刻"——每帧用 RAF 时间戳驱动软件锁相环，输出均匀单调的
// 栅格 tick（= Choreographer 采样的等价物）。Worker 线程（计时线程
// tTVPTimerThread::Execute 等）不受影响，仍读原始时钟。RAF 停摆（模态
// 自旋循环 / 标签页隐藏）超过 50ms 自动回退原始时钟。
// 调查记录见 analysis/Timer_Event_Dispatch_Chain_libkrkr2so.md。
static double TVPWebSnapTick = -1; // 当前帧锁相 tick（仅主线程读写）
static double TVPWebSnapPeriod = 0; // 锁相周期估计（帧栅格间距 ms）
static double TVPWebSnapUpdRaw = -1; // 上次锁相更新时的原始时钟（停摆检测）
static double TVPWebLastRafT = -1; // 上一帧 RAF 时间戳
static tjs_uint32 TVPWebMainLastTick = 0; // 主线程返回值单调护栏

// 注意：本机 emscripten 的 CLOCK_MONOTONIC 是 timeOrigin 绝对毫秒（~1.78e12），
// RAF 时间戳是页面相对毫秒——锁相环统一在绝对域运行（rafT + performance.timeOrigin）。
// double→tjs_uint32 必须经 uint64 中转：wasm 的 trunc_sat 会把超出 uint32 范围的
// double 饱和成 0xFFFFFFFF，而引擎语义是 mod 2^32 回卷（原整数算术行为）。
static inline double TVPWebRawNowMS() {
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
    return 0;
}

static inline tjs_uint32 TVPWebMSToTick32(double ms) {
    return static_cast<tjs_uint32>(static_cast<tjs_uint64>(ms));
}

// 每帧帧首由 TVPMainScene::update（cocos RAF 回调内）调用
void TVPWebFrameTickUpdate() {
    // 首次调用时包裹 window.requestAnimationFrame 以捕获 vsync 对齐的
    // RAF 时间戳（emscripten/cocos 主循环每帧重新注册 RAF，下一帧起生效）
    double t = EM_ASM_DOUBLE({
        if(!globalThis.__tvpRafWrapped) {
            globalThis.__tvpRafWrapped = 1;
            globalThis.__tvpRafT = -1;
            var orig = window.requestAnimationFrame.bind(window);
            window.requestAnimationFrame = function(cb) {
                return orig(function(ts) {
                    globalThis.__tvpRafT = ts;
                    cb(ts);
                });
            };
        }
        // 换算到绝对时钟域（与 CLOCK_MONOTONIC/emscripten_get_now 一致）
        return globalThis.__tvpRafT < 0
            ? -1
            : globalThis.__tvpRafT + performance.timeOrigin;
    });
    TVPWebSnapUpdRaw = TVPWebRawNowMS();
    if(t < 0 || t == TVPWebLastRafT)
        return;
    double prev = TVPWebLastRafT;
    TVPWebLastRafT = t;
    if(prev < 0)
        return;
    double d = t - prev;
    if(d < 2 || d > 250) {
        // 离群帧间隔（标签页恢复 / 长卡顿）：丢弃锁相状态重新同步
        TVPWebSnapTick = -1;
        TVPWebSnapPeriod = 0;
        return;
    }
    if(TVPWebSnapPeriod <= 0 || TVPWebSnapTick < 0) {
        TVPWebSnapPeriod = d;
        TVPWebSnapTick = t;
        return;
    }
    double n = floor(d / TVPWebSnapPeriod + 0.5);
    if(n < 1)
        n = 1;
    double slot = d / n;
    if(fabs(slot - TVPWebSnapPeriod) < TVPWebSnapPeriod * 0.25)
        TVPWebSnapPeriod += 0.05 * (slot - TVPWebSnapPeriod);
    double pred = TVPWebSnapTick + n * TVPWebSnapPeriod;
    double err = t - pred;
    if(fabs(err) <= 2.5)
        pred += err * 0.1; // 栅格内：慢相位跟踪，滤除 ±1-2ms 时间戳抖动
    else
        pred = t; // 失锁：重同步到当前时间戳
    if(pred <= TVPWebSnapTick)
        pred = TVPWebSnapTick + 0.01; // 单调
    TVPWebSnapTick = pred;
}

tjs_uint32 TVPGetRoughTickCount32() {
    double raw = TVPWebRawNowMS();
    if(emscripten_is_main_runtime_thread()) {
        tjs_uint32 r;
        if(TVPWebSnapTick >= 0 && raw - TVPWebSnapUpdRaw <= 50.0) {
            r = TVPWebMSToTick32(TVPWebSnapTick);
        } else {
            // 启动期 / RAF 停摆：原始时钟
            r = TVPWebMSToTick32(raw);
        }
        // 锁相域与原始域相差 ±数 ms，切换时钳制保证主线程单调；
        // 回退幅度达 2^31 视为 mod 2^32 真回卷，放行（TickCount.cpp 补偿）
        if(r < TVPWebMainLastTick && (TVPWebMainLastTick - r) < 0x80000000UL)
            r = TVPWebMainLastTick;
        TVPWebMainLastTick = r;
        return r;
    }
    return TVPWebMSToTick32(raw);
}

void TVPExitApplication(int code) {
    // On web, don't actually exit — emscripten_force_exit breaks the runtime
    // without EXIT_RUNTIME set. Game scripts (e.g. keybinder.tjs exception
    // handlers) may call System.exit() for non-fatal errors that the game
    // can survive. Log the exit request and continue.
    spdlog::warn("TVPExitApplication({}) called — ignored on web build", code);
    TVPDeliverCompactEvent(TVP_COMPACT_LEVEL_MAX);
}

static bool tryStartFromDir(const std::string &dir) {
    std::string dataXp3, xp3File;
    bool hasStartupTjs = false;

    // TVPListDir 合并枚举 VLFS（游戏文件）与 MEMFS（零碎小文件）
    TVPListDir(dir, [&](const std::string &name, int mode) {
        if (name.empty() || name[0] == '.' || !(mode & S_IFREG)) return;

        std::string full = dir;
        if (full.back() != '/') full += '/';
        full += name;

        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (lower == "data.xp3") {
            dataXp3 = full;
        } else if (lower.size() > 4 &&
                   lower.compare(lower.size() - 4, 4, ".xp3") == 0 &&
                   xp3File.empty()) {
            xp3File = full;
        } else if (lower == "startup.tjs") {
            hasStartupTjs = true;
        }
    });

    if (!dataXp3.empty()) {
        spdlog::info("Found {}, auto-starting game", dataXp3);
        TVPMainScene::GetInstance()->startupFrom(dataXp3);
        return true;
    }
    if (!xp3File.empty()) {
        spdlog::info("Found {}, auto-starting game", xp3File);
        TVPMainScene::GetInstance()->startupFrom(xp3File);
        return true;
    }
    if (hasStartupTjs) {
        spdlog::info("Found startup.tjs in {}, auto-starting game", dir);
        TVPMainScene::GetInstance()->startupFrom(dir);
        return true;
    }
    return false;
}

EM_JS(char *, krkr2_get_startup_xp3_path, (), {
    if (Module._startupXp3Path) {
        var s = Module._startupXp3Path;
        var len = lengthBytesUTF8(s) + 1;
        var buf = _malloc(len);
        stringToUTF8(s, buf, len);
        return buf;
    }
    return 0;
});

bool TVPCheckStartupArg() {
    TVPInstallLayerFrameDumperIfRequested();

    char *selectedXp3 = krkr2_get_startup_xp3_path();
    if (selectedXp3) {
        std::string path(selectedXp3);
        free(selectedXp3);
        // libkrkr2.so 数据流：TVPAddAutoPath(0x8EB4B4) 唯一调用者是
        // Storages.addAutoPath 的 NCB 绑定(0x8EDF80)，引擎启动流不预挂任何
        // 同级 xp3。此前 Web 独有的 autoMountSiblingXp3 会在脚本运行前把
        // patch.xp3 等加入 autopath 列表前部，而 TVPAddAutoPath 对重复项
        // no-op（保留早位置），破坏 Initialize.tjs "patch 最后挂载=最高
        // 优先" 的覆盖语义（汉化补丁失效，如 夏空彼方KR）。挂载顺序必须
        // 完全由游戏脚本决定。
        TVPMainScene::GetInstance()->startupFrom(path);
        return true;
    }

    if (tryStartFromDir("/")) return true;

    std::vector<std::string> subdirs;
    TVPListDir("/", [&](const std::string &name, int mode) {
        if (name.empty() || name[0] == '.') return;
        if (name == "dev" || name == "proc" || name == "tmp" ||
            name == "home" || name == "save") return;
        if (mode & S_IFDIR) subdirs.push_back("/" + name);
    });
    for (const auto &full : subdirs) {
        if (tryStartFromDir(full)) return true;
    }
    return false;
}

void TVPProcessInputEvents() {}

bool TVPDeleteFile(const std::string &filename) {
    if (VLFS::Enabled() && VLFS::Has(filename.c_str()) == 1) {
        bool ok = VLFS::Unlink(filename.c_str()) == 0;
        unlink(filename.c_str()); // MEMFS 镜像（如有）一并清理
        return ok;
    }
    return unlink(filename.c_str()) == 0;
}

bool TVPCopyFile(const std::string &from, const std::string &to);

bool TVPRenameFile(const std::string &from, const std::string &to) {
    if (VLFS::Enabled() && VLFS::Has(from.c_str()) == 1) {
        // VLFS 无原生 rename（罕见路径，如 XP3 重打包）：复制 + 删除
        if (!TVPCopyFile(from, to)) return false;
        return TVPDeleteFile(from);
    }
    return rename(from.c_str(), to.c_str()) == 0;
}

bool TVPCopyFile(const std::string &from, const std::string &to) {
    if (VLFS::Enabled() && VLFS::Has(from.c_str()) == 1) {
        int src = VLFS::Open(from.c_str(), 0);
        if (src < 0) return false;
        int dst = VLFS::Open(to.c_str(), 1);
        if (dst < 0) {
            VLFS::Close(src);
            return false;
        }
        char buf[65536];
        int n;
        while ((n = VLFS::Read(src, buf, sizeof(buf))) > 0) {
            VLFS::Write(dst, buf, n);
        }
        VLFS::Close(src);
        VLFS::Close(dst); // onWriteClose 钩子负责持久化/镜像
        return n >= 0;
    }

    FILE *src = fopen(from.c_str(), "rb");
    if (!src) {
        spdlog::error("TVPCopyFile fopen src FAILED: {}", from);
        return false;
    }

    // MEMFS 源 → VLFS 写路径（统一持久化语义）
    if (VLFS::Enabled()) {
        int dst = VLFS::Open(to.c_str(), 1);
        if (dst >= 0) {
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
                VLFS::Write(dst, buf, (int)n);
            }
            fclose(src);
            VLFS::Close(dst);
            return true;
        }
    }

    FILE *dst = fopen(to.c_str(), "wb");
    if (!dst) {
        spdlog::error("TVPCopyFile fopen dst FAILED: {}", to);
        fclose(src);
        return false;
    }

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        fwrite(buf, 1, n, dst);
    }
    fclose(src);
    fclose(dst);
    return true;
}

void TVPSendToOtherApp(const std::string &filename) {}

std::vector<std::string> TVPGetDriverPath() { return {"/"}; }

std::vector<std::string> TVPGetAppStoragePath() {
    return {"/save/"};
}

const std::string &TVPGetInternalPreferencePath() {
    static std::string path = "/save/";
    return path;
}

bool TVPWriteDataToFile(const ttstr &filepath, const void *data,
                        unsigned int len) {
    std::string path = filepath.AsStdString();

    // VLFS overlay 写入；onWriteClose 钩子（shell.html）负责 IndexedDB
    // write-through、MEMFS 小文件镜像（遗留 fopen 读兼容）、FSA 回写
    if (VLFS::Enabled()) {
        int fd = VLFS::Open(path.c_str(), 1);
        if (fd >= 0) {
            unsigned int total = 0;
            while (total < len) {
                int n = VLFS::Write(fd, (const char *)data + total, (int)(len - total));
                if (n <= 0) break;
                total += n;
            }
            VLFS::Close(fd);
            if (total == len) return true;
            spdlog::error("TVPWriteDataToFile VLFS short write: {}", path);
            return false;
        }
    }

    FILE *handle = fopen(path.c_str(), "wb");
    if (handle) {
        bool ret = fwrite(data, 1, len, handle) == len;
        fclose(handle);
        return ret;
    }
    spdlog::error("TVPWriteDataToFile fopen FAILED: {}", path);
    return false;
}

void TVPShowIME(int x, int y, int w, int h) {}
void TVPHideIME() {}

void TVPPrintLog(const char *str) {
    emscripten_log(EM_LOG_CONSOLE, "%s", str);
}

void TVPCheckMemory() {}

int TVPShowSimpleMessageBox(const ttstr &text, const ttstr &caption) {
    std::vector<ttstr> btns;
    btns.emplace_back(TJS_W("OK"));
    return TVPShowSimpleMessageBox(text, caption, btns);
}

int TVPShowSimpleMessageBoxYesNo(const ttstr &text, const ttstr &caption) {
    std::vector<ttstr> btns;
    btns.emplace_back(TJS_W("Yes"));
    btns.emplace_back(TJS_W("No"));
    return TVPShowSimpleMessageBox(text, caption, btns);
}

#endif // EMSCRIPTEN
