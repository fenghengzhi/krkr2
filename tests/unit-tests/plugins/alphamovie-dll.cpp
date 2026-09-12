#include <memory>
#include <catch2/catch_test_macros.hpp>
#include "ncbind.hpp"

extern tTJS *TVPScriptEngine;

namespace {
struct ReleaseDispatch {
    void operator()(iTJSDispatch2 *object) const { object->Release(); }
};
using DispatchOwner = std::unique_ptr<iTJSDispatch2, ReleaseDispatch>;

DispatchOwner CreateMovie() {
    static bool loaded = false;
    if(!TVPScriptEngine)
        TVPScriptEngine = new tTJS();
    if(!loaded) {
        ncbAutoRegister::AllRegist();
        REQUIRE(ncbAutoRegister::LoadModule(TJS_W("alphamovie.dll")));
        loaded = true;
    }
    auto *global = TVPScriptEngine->GetGlobalNoAddRef();
    tTJSVariant cls;
    REQUIRE(global->PropGet(0, TJS_W("AlphaMovie"), nullptr, &cls, global) ==
            TJS_S_OK);
    iTJSDispatch2 *movie = nullptr;
    REQUIRE(cls.AsObjectNoAddRef()->CreateNew(0, nullptr, nullptr, &movie,
                                             0, nullptr, global) == TJS_S_OK);
    REQUIRE(movie != nullptr);
    return DispatchOwner(movie);
}

tjs_int64 GetInteger(iTJSDispatch2 *movie, const tjs_char *name) {
    tTJSVariant value;
    REQUIRE(movie->PropGet(0, name, nullptr, &value, movie) == TJS_S_OK);
    return (tjs_int64)value;
}

void SetInteger(iTJSDispatch2 *movie, const tjs_char *name, int value) {
    tTJSVariant input(value);
    REQUIRE(movie->PropSet(0, name, nullptr, &input, movie) == TJS_S_OK);
}
} // namespace

TEST_CASE("AlphaMovie preserves unopened instance and stop boundaries",
          "[alphamovie]") {
    auto owner = CreateMovie();
    auto *movie = owner.get();
    tTJSVariant result;
    REQUIRE(movie->FuncCall(0, TJS_W("isPlaying"), nullptr, &result, 0,
                           nullptr, movie) == TJS_S_OK);
    REQUIRE((tjs_int)result == 1);
    REQUIRE(GetInteger(movie, TJS_W("loop")) == 1);
    REQUIRE(GetInteger(movie, TJS_W("nextLoop")) == 1);
    REQUIRE(GetInteger(movie, TJS_W("left")) == 0);
    REQUIRE(GetInteger(movie, TJS_W("top")) == 0);
    REQUIRE(GetInteger(movie, TJS_W("preloadSamples")) == 5);

    tTJSVariant nullLayer(static_cast<iTJSDispatch2 *>(nullptr));
    tTJSVariant *imageArgs[] = {&nullLayer};
    REQUIRE(movie->FuncCall(0, TJS_W("showNextImage"), nullptr, &result, 1,
                           imageArgs, movie) == TJS_S_OK);
    REQUIRE((tjs_int)result == 0);

    tTJSVariant left(-7), top(11);
    tTJSVariant *positionArgs[] = {&left, &top};
    REQUIRE(movie->FuncCall(0, TJS_W("setPosition"), nullptr, &result, 2,
                           positionArgs, movie) == TJS_S_OK);
    REQUIRE(result.Type() == tvtVoid);
    REQUIRE(GetInteger(movie, TJS_W("left")) == -7);
    REQUIRE(GetInteger(movie, TJS_W("top")) == 11);

    REQUIRE(movie->FuncCall(0, TJS_W("clear"), nullptr, nullptr, 0,
                           nullptr, movie) == TJS_S_OK);
    REQUIRE(movie->FuncCall(0, TJS_W("isPlaying"), nullptr, &result, 0,
                           nullptr, movie) == TJS_S_OK);
    REQUIRE((tjs_int)result == 1);
    REQUIRE(movie->FuncCall(0, TJS_W("stop"), nullptr, nullptr, 0,
                           nullptr, movie) == TJS_S_OK);
    REQUIRE(movie->FuncCall(0, TJS_W("isPlaying"), nullptr, &result, 0,
                           nullptr, movie) == TJS_S_OK);
    REQUIRE((tjs_int)result == 0);
}

TEST_CASE("AlphaMovie keeps dispatch errors and preload limits",
          "[alphamovie]") {
    auto owner = CreateMovie();
    auto *movie = owner.get();
    for(const auto *name : {TJS_W("open"), TJS_W("showNextImage"),
                            TJS_W("setPosition"), TJS_W("setNextMovieFile")})
        REQUIRE(movie->FuncCall(0, name, nullptr, nullptr, 0, nullptr, movie) ==
                TJS_E_BADPARAMCOUNT);
    tTJSVariant value(1);
    for(const auto *name : {TJS_W("numOfFrame"), TJS_W("screenWidth"),
                            TJS_W("screenHeight"), TJS_W("FPSScale"),
                            TJS_W("FPSRate")})
        REQUIRE(movie->PropSet(0, name, nullptr, &value, movie) ==
                TJS_E_ACCESSDENYED);
    SetInteger(movie, TJS_W("preloadSamples"), 1);
    REQUIRE(GetInteger(movie, TJS_W("preloadSamples")) == 1);
    SetInteger(movie, TJS_W("preloadSamples"), 30);
    REQUIRE(GetInteger(movie, TJS_W("preloadSamples")) == 30);
    for(int count : {-1, 0, 31}) {
        SetInteger(movie, TJS_W("preloadSamples"), count);
        REQUIRE(GetInteger(movie, TJS_W("preloadSamples")) == 30);
    }
    SetInteger(movie, TJS_W("loop"), 0);
    SetInteger(movie, TJS_W("nextLoop"), -1);
    REQUIRE(GetInteger(movie, TJS_W("loop")) == 0);
    REQUIRE(GetInteger(movie, TJS_W("nextLoop")) == 1);
}
