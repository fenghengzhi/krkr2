#include <spdlog/spdlog.h>
#include "AppDelegate.h"

#include "MainScene.h"
#include "Application.h"
#include "Platform.h"
#include "2d/CCCamera.h"
#include "ui/GlobalPreferenceForm.h"
#include "ui/MainFileSelectorForm.h"
#include "ui/extension/UIExtension.h"
#include "ConfigManager/LocaleConfigManager.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
extern unsigned int TVPMaxTextureSize;
#endif

static cocos2d::Size designSize(960, 640);
extern std::thread::id TVPMainThreadID;

extern "C" void SDL_SetMainReady();

bool TVPCheckStartupArg();

std::string TVPGetCurrentLanguage();

void TVPAppDelegate::applicationWillEnterForeground() {
    ::Application->OnActivate();
    cocos2d::Director::getInstance()->startAnimation();
}

void TVPAppDelegate::applicationDidEnterBackground() {
    ::Application->OnDeactivate();
    cocos2d::Director::getInstance()->stopAnimation();
}

bool TVPAppDelegate::applicationDidFinishLaunching() {
    SDL_SetMainReady();
    TVPMainThreadID = std::this_thread::get_id();
    spdlog::debug("App Finish Launching");
    // initialize director
    auto director = cocos2d::Director::getInstance();
    auto glview = director->getOpenGLView();
    if(!glview) {
        glview = cocos2d::GLViewImpl::create("krkr2");
        director->setOpenGLView(glview);
#if CC_TARGET_PLATFORM == CC_PLATFORM_WIN32
        HWND hwnd = glview->getWin32Window();
        if(hwnd) {
            // 添加可调节边框和最大化按钮
            LONG style = GetWindowLong(hwnd, GWL_STYLE);
            style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
            SetWindowLong(hwnd, GWL_STYLE, style);
        }
#endif
    }

#ifdef __EMSCRIPTEN__
    // Android sub_A587D4 @ 0xA587D4 initializes this shared renderer limit
    // from GL_MAX_TEXTURE_SIZE before motionplayer's sub_695DE8 consumes it.
    // Web uses the software TVP render manager, so its OpenGL InitGL path is
    // not constructed; bridge the same GL-context value at context creation.
    GLint maxTextureSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    TVPMaxTextureSize = static_cast<unsigned int>(maxTextureSize);
#endif

#if (CC_TARGET_PLATFORM == CC_PLATFORM_ANDROID ||                              \
     CC_TARGET_PLATFORM == CC_PLATFORM_IOS)
    // Set the design resolution
    cocos2d::Size screenSize = glview->getFrameSize();
    if(screenSize.width < screenSize.height) {
        std::swap(screenSize.width, screenSize.height);
    }
    cocos2d::Size ds = designSize;
    ds.height = ds.width * screenSize.height / screenSize.width;
    glview->setDesignResolutionSize(screenSize.width, screenSize.height,
                                    ResolutionPolicy::EXACT_FIT);
#else
    glview->setDesignResolutionSize(designSize.width, designSize.height,
                                    ResolutionPolicy::FIXED_WIDTH);
#endif

    {
        auto frameSize = glview->getFrameSize();
        auto vp = cocos2d::experimental::Viewport(
            0, 0,
            static_cast<int>(frameSize.width),
            static_cast<int>(frameSize.height));
        cocos2d::Camera::setDefaultViewport(vp);
    }

    std::vector<std::string> searchPath;

    // In this demo, we select resource according to the frame's
    // height. If the resource size is different from design
    // resolution size, you need to set contentScaleFactor. We use the
    // ratio of resource's height to the height of design resolution,
    // this can make sure that the resource's height could fit for the
    // height of design resolution.
    searchPath.emplace_back("res");

    // set searching path
    cocos2d::FileUtils::getInstance()->setSearchPaths(searchPath);

#ifdef __EMSCRIPTEN__
    bool showStats = EM_ASM_INT({
        return new URLSearchParams(window.location.search).has('debug') ? 1 : 0;
    });
    director->setDisplayStats(showStats);
#else
    director->setDisplayStats(true);
#endif

    // set FPS. the default value is 1.0/60 if you don't call this
    director->setAnimationInterval(1.0f / 60);

    TVPInitUIExtension();

    // initialize something
    LocaleConfigManager::GetInstance()->Initialize(TVPGetCurrentLanguage());
    // create a scene. it's an autorelease object
    TVPMainScene *scene = TVPMainScene::CreateInstance();

    // run
    director->runWithScene(scene);

    scene->scheduleOnce(
        [](float dt) {
            TVPMainScene::GetInstance()->unschedule("launch");
            TVPGlobalPreferenceForm::Initialize();
            if(!TVPCheckStartupArg()) {
                TVPMainScene::GetInstance()->pushUIForm(
                    TVPMainFileSelectorForm::create());
            }
        },
        0, "launch");

    return true;
}

void TVPAppDelegate::initGLContextAttrs() {
    GLContextAttrs glContextAttrs = { 8, 8, 8, 8, 24, 8 };
    cocos2d::GLView::setGLContextAttrs(glContextAttrs);
}


void TVPOpenPatchLibUrl() {
    cocos2d::Application::getInstance()->openURL(
        "https://zeas2.github.io/Kirikiroid2_patch/patch");
}
