#pragma once

#ifdef __EMSCRIPTEN__

// Enables a continuous-event hook that emits one JSON line per frame per
// layer, tagged with the prefix "LAYER_FRAME". Gated by the URL query
// parameter `dumpLayers=1`. Safe to call multiple times; only the first
// call installs the hook.
void TVPInstallLayerFrameDumperIfRequested();

#endif
