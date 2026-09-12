#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Intentional game compatibility correction, not reference-source recovery.
// Some NEKOPARA scene packs omit env.objectList while retaining the rendered
// characters. Their touch script consequently registers no actions. Match the
// known complete script before restoring its missing touch environment.
// Evidence and scope: analysis/nekopara3_touch_registration_compatibility_2026-09-12.md.
inline std::u16string TVPPrepareNekoparaTouchScript(
    std::u16string_view shortname, std::u16string_view source) {
    if(shortname != u"nw_touchmode.tjs")
        return {};

    std::uint64_t fingerprint = UINT64_C(14695981039346656037);
    std::size_t length = 0;
    for(const char16_t ch : source) {
        if(ch == u'\r')
            continue;
        ++length;
        fingerprint = (fingerprint ^ (ch & 0xffu)) * UINT64_C(1099511628211);
        fingerprint = (fingerprint ^ (ch >> 8u)) * UINT64_C(1099511628211);
    }
    if(length != 9019 || fingerprint != UINT64_C(0xa1642438f5f2ba95))
        return {};

    constexpr std::u16string_view anchor = u"var list = world_object.env.layerList;";
    const auto insertion = source.find(anchor);
    if(insertion == std::u16string_view::npos)
        return {};

    constexpr auto fallback = uR"TJS(
    // Restore the visible state, not the scene's earlier entrance animation.
    if (world_object.env.layerList.count == 0) {
        var liveLayers = world_object.envlayerList;
        for (var index = 0; index < liveLayers.count; index++) {
            var liveLayer = liveLayers[index];
            if (!liveLayer.visible || !liveLayer.targetLayer) continue;
            var target = liveLayer.targetLayer;
            var image = target._image;
            if (!image || !image.isEmote()) continue;
            var object = world_object.env.createEnvObject(liveLayer.name,
                                                         liveLayer.className);
            var state = %[name:liveLayer.name, cname:liveLayer.className,
                          disp:2, imageFile:liveLayer.imageFile,
                          imageOptions:Scripts.clone(liveLayer.imageOptions),
                          imageOptionsAll:Scripts.clone(liveLayer.imageOptions),
                          actionList:[], cpropsact:%[]];
            var properties = ["xpos", "ypos", "zpos", "zoomx", "zoomy",
                              "offsetx", "offsety", "rotate", "opacity",
                              "leveloffset"];
            for (var propertyIndex = 0; propertyIndex < properties.count;
                 propertyIndex++) {
                var propertyName = properties[propertyIndex];
                state.actionList.add([propertyName, target[propertyName]]);
                state.cpropsact[propertyName] = 1;
            }
            var player = image._player;
            var variables = player.variableKeys;
            for (var variableIndex = 0; variableIndex < variables.count;
                 variableIndex++) {
                var variableName = variables[variableIndex];
                state.actionList.add(["$" + variableName,
                                      player.getVariable(variableName)]);
                state.cpropsact["$" + variableName] = 1;
            }
            object.onRestore(state);
        }
    }
    )TJS";
    std::u16string patched(source);
    patched.insert(insertion, fallback);
    return patched;
}
