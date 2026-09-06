#pragma once

// Optional Wasmtime host diagnostics. Browser/native builds have no URL query
// or host call on this path; the engine's trace sites remain available to the
// differential guest without coupling them to embedded JavaScript text.
#if defined(__EMSCRIPTEN__) && defined(KRKR2_WASMTIME_DIAGNOSTICS)
extern "C" int krkr2_wasmtime_logo_trace_enabled()
    __attribute__((import_module("env"),
                   import_name("krkr2_wasmtime_logo_trace_enabled")));

static inline bool TVPLogoTraceEnabled() {
    return krkr2_wasmtime_logo_trace_enabled() != 0;
}
#else
static constexpr bool TVPLogoTraceEnabled() {
    return false;
}
#endif
