/// @file TicoMain.cpp
/// @brief Entry point for tico-integrated mgba NRO
/// Sets up SDL/Vulkan/ImGui and runs the main loop

#include "TicoCore.h"
#include "../gbastation/session_host.hpp"
#include "../gbastation/menu_host.hpp"
#ifdef WOPLAY_HOST
#include "woplay/ui_components/binding_capture.hpp"
#endif
#include <clocale>
#include "TicoSession.h"
#include "TicoSafeFile.h"
#include "UsbStorage.h"
#include "TicoConfig.h"
#include "TicoAudio.h"
#include "TicoShaderChain.h"
#include "TicoVulkan.h"
#include "overlay/imgui_overlay.h"
#include "overlay/overlay_ui.h"
#include "overlay/tico_config.h"
#include "overlay/translation_manager.h"

#include <SDL.h>
#include <dirent.h>
#include <json.hpp>
#include <strings.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <cctype>
#include <cmath>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <cstring>
#include "TicoUtils.h"
#include "deps/stb/stb_image.h"
#include "deps/stb/stb_image_write.h"
#include <array>
#include <ctime>
#include <vector>
#include "TicoLogger.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include <SDL_vulkan.h>

//==============================================================================
// NX System Configuration (extern "C")
//==============================================================================

#ifdef __SWITCH__
extern "C" {
u32 __NvOptimusEnablement = 1;
u32 __NvDeveloperOption = 1;
u32 __nx_applet_type = AppletType_Application;
u32 __nx_applet_exit_mode = 0; // 0 = standard exit (return to Homebrew ABI loader if NRO). 1 = forceful applet exit
size_t __nx_heap_size = 0;
}
#endif

//==============================================================================
// Globals
//==============================================================================

static SDL_Window *g_window = nullptr;
static std::unique_ptr<TicoShaderChain> g_chain;
static std::string g_activePreset = "\x01"; // forces the first load

namespace OverlayUI = SwitchFrontend::OverlayUI;
namespace ImGuiOverlay = SwitchFrontend::ImGuiOverlay;
namespace OverlayConfig = SwitchFrontend::TicoConfig;

static std::unique_ptr<TicoCore> g_core;

// Started without a game: the library lists the ROM folders, and leaving a
// game returns to it instead of chainloading tico.
static bool g_standalone = false;
static std::string g_pendingLaunch;
static void ShowLibrary();
static void OpenMenu();
static void CloseMenu();
static bool g_waitGameInput = false;
static bool g_woplayAutoSaved = false;
static void StartGame(const std::string &slug, const std::string &romArg, const std::string &titleArg);
// the title the running game was started with, for Restart
static std::string g_titleArg;

// Quick menu
static bool g_menuOpen = false;
static void RunWoPlayState(const woplay_host::StateRequest& request);
static bool g_overlayReady = false;
static bool g_toggleHeld = false;
static uint32_t g_navHeldPrev = 0;
static int g_navRepeatFrames = 0;
static constexpr int kNavInitialDelayFrames = 14;
static constexpr int kNavRepeatFrames = 6;

// Fast forward (Display > Fast Forward): the hotkey, held or toggled, runs the
// core at fast_forward_speed by emulating extra frames per presented one;
// "unlimited" drops vsync instead.
static bool g_ffHotkeyHeld = false;
static bool g_ffLatched = false;
static float g_ffFrameBudget = 0.0f;
static void StopFastForward();

// HUD frame counter
static int g_hudFrames = 0;
static float g_hudSeconds = 0.0f;
static float g_hudFps = 0.0f;

static bool g_running = true;
static TicoAudio g_audio;
static SDL_AudioDeviceID g_audioDevice = 0;
static SDL_GameController *g_controllers[4] = {nullptr, nullptr, nullptr, nullptr};
static bool g_controllersDirty = true;



#ifdef __SWITCH__
static u8 g_lastOperationMode = 255;

static void PinCurrentThreadToCore(int core, const char *label)
{
    if (core < 0 || core > 2)
        return;

    Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
    if (R_FAILED(rc))
    {
        LOG_WARN("HOME", "Failed to pin %s thread to core %d (rc=0x%x)", label, core, rc);
    }
    else
    {
        LOG_INFO("HOME", "Pinned %s thread to core %d", label, core);
    }
}

static bool UpdateScreenMode()
{
    u8 operationMode = appletGetOperationMode();
    if (operationMode == g_lastOperationMode)
        return false;

    // Size the window to the mode and crop from the top-left, as tico-dolphin
    // does, so the swapchain always matches what is on screen.
    const bool handheld = operationMode == AppletOperationMode_Handheld;
    const u32 w = handheld ? 1280 : 1920, h = handheld ? 720 : 1080;
    nwindowSetDimensions(nwindowGetDefault(), w, h);
    nwindowSetCrop(nwindowGetDefault(), 0, 0, w, h);
    TicoVulkan::Resize(w, h);
    LOG_INFO("DISPLAY", "Mode → %s (%ux%u)", handheld ? "Handheld" : "Docked", w, h);
    if (ImGui::GetCurrentContext()) {
        ImGui::GetIO().FontGlobalScale = handheld ? 1.0f : 1.5f;
    }
    g_lastOperationMode = operationMode;
    return true;
}
#endif

//==============================================================================
// SDL/Vulkan Initialization
//==============================================================================

static void CloseControllers()
{
    for (SDL_GameController *&controller : g_controllers)
    {
        if (controller)
        {
            SDL_GameControllerClose(controller);
            controller = nullptr;
        }
    }
}

static void RefreshControllers()
{
    CloseControllers();

    int controllerIndex = 0;
    int joystickCount = SDL_NumJoysticks();

    for (int i = 0; i < joystickCount && controllerIndex < 4; ++i)
    {
        if (!SDL_IsGameController(i))
            continue;

        SDL_GameController *controller = SDL_GameControllerOpen(i);
        if (!controller)
        {
            LOG_WARN("INPUT", "Failed to open controller %d: %s", i, SDL_GetError());
            continue;
        }

        g_controllers[controllerIndex++] = controller;
    }

    g_controllersDirty = false;
}

static void GetDisplayResolution(int &w, int &h)
{
#ifdef __SWITCH__
    u8 opMode = appletGetOperationMode();
    if (opMode == AppletOperationMode_Handheld)
    {
        w = 1280;
        h = 720;
    }
    else
    {
        w = 1920;
        h = 1080;
    }
#else
    uint32_t sw = 0, sh = 0;
    TicoVulkan::GetSwapExtent(sw, sh);
    if (sw && sh)
    {
        w = (int)sw;
        h = (int)sh;
    }
    else if (g_window)
        SDL_GetWindowSize(g_window, &w, &h);
    else
    {
        w = 1280;
        h = 720;
    }
#endif
}

bool InitWindow()
{
    LOG_INFO("HOME", "Starting initialization...");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER |
                 SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0)
    {
        LOG_ERROR("HOME", "SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    LOG_INFO("HOME", "SDL initialized");

#ifdef __SWITCH__
    g_window = nullptr;
    LOG_INFO("HOME", "Switch: skipping SDL window (using native window)");

    UpdateScreenMode();
    int w, h;
    GetDisplayResolution(w, h);
    LOG_INFO("HOME", "Switch Resolution: %dx%d (logical)", w, h);

#else
#if defined(__APPLE__) && defined(WOPLAY_HOST)
    const auto* library = std::getenv("GBS_VULKAN_LIBRARY");
    if (SDL_Vulkan_LoadLibrary(library ? library : WOPLAY_MOLTENVK) != 0) {
        std::fprintf(stderr, "MoltenVK: %s\n", SDL_GetError());
        return false;
    }
#endif
    g_window = SDL_CreateWindow(woplay_host::active() ? "WoPlay - mGBA" : "mgba",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                TicoConfig::WINDOW_WIDTH, TicoConfig::WINDOW_HEIGHT,
                                SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);

    if (!g_window)
    {
        LOG_ERROR("HOME", "SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
#endif

    if (TicoConfig::USE_SDLQUEUEAUDIO)
    {
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = TicoAudio::SAMPLE_RATE;
        want.format = AUDIO_S16SYS;
        want.channels = TicoAudio::CHANNELS;
        want.samples = 2048;
        want.callback = NULL;

        g_audioDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (g_audioDevice == 0)
        {
            LOG_ERROR("AUDIO", "SDL_OpenAudioDevice failed: %s", SDL_GetError());
        }
        else
        {
            LOG_INFO("AUDIO", "SDL_QueueAudio initialized. DeviceID: %d, Freq: %d", g_audioDevice, have.freq);
        }
    }
    else
    {
        // At the rate TicoAudio resamples the core to, or the sound plays slower
        // and lower, and the overfull buffer drops batches (crackling)
        if (Mix_OpenAudio(TicoAudio::SAMPLE_RATE, AUDIO_S16SYS, TicoAudio::CHANNELS, 1024) < 0)
        {
            LOG_ERROR("AUDIO", "Mix_OpenAudio failed: %s", Mix_GetError());
        }
        else
        {
            LOG_INFO("AUDIO", "SDL_mixer initialized");
        }
    }

    return true;
}

static void AudioSampleCallback(int16_t left, int16_t right)
{
    g_audio.PushSample(left, right);
}

static size_t AudioSampleBatchCallback(const int16_t *data, size_t frames)
{
    return g_audio.PushSamples(data, frames);
}

// The core's last frame, kept for the picture saved with a save state.
static struct
{
    std::vector<uint8_t> data;
    unsigned width = 0;
    unsigned height = 0;
    size_t pitch = 0;
    retro_pixel_format format = RETRO_PIXEL_FORMAT_RGB565;
} g_lastFrame;

static void VideoCallback(const void *data, unsigned width, unsigned height, size_t pitch,
                          retro_pixel_format format)
{
    if (g_chain)
        g_chain->SetSourceFrame(data, width, height, pitch, format);
    if (data) // null repeats the previous frame
    {
        g_lastFrame.data.assign((const uint8_t *)data, (const uint8_t *)data + pitch * height);
        g_lastFrame.width = width;
        g_lastFrame.height = height;
        g_lastFrame.pitch = pitch;
        g_lastFrame.format = format;
    }
}

// One pixel of the last frame as RGBA.
static void FramePixel(unsigned x, unsigned y, uint8_t *out)
{
    const uint8_t *row = g_lastFrame.data.data() + y * g_lastFrame.pitch;
    switch (g_lastFrame.format)
    {
    case RETRO_PIXEL_FORMAT_XRGB8888:
    {
        uint32_t p;
        memcpy(&p, row + x * 4, 4);
        out[0] = (p >> 16) & 0xFF;
        out[1] = (p >> 8) & 0xFF;
        out[2] = p & 0xFF;
        break;
    }
    case RETRO_PIXEL_FORMAT_0RGB1555:
    {
        uint16_t p;
        memcpy(&p, row + x * 2, 2);
        out[0] = ((p >> 10) & 0x1F) * 255 / 31;
        out[1] = ((p >> 5) & 0x1F) * 255 / 31;
        out[2] = (p & 0x1F) * 255 / 31;
        break;
    }
    default: // RGB565
    {
        uint16_t p;
        memcpy(&p, row + x * 2, 2);
        out[0] = ((p >> 11) & 0x1F) * 255 / 31;
        out[1] = ((p >> 5) & 0x3F) * 255 / 63;
        out[2] = (p & 0x1F) * 255 / 31;
        break;
    }
    }
    out[3] = 255;
}

// Saves the last frame beside a save state, shrunk to fit 256x192 (box
// filtered), just big enough for the Save/Load State panel.
static bool SaveStatePicture(const std::string &path)
{
    if (g_lastFrame.data.empty() || !g_lastFrame.width || !g_lastFrame.height)
        return false;
    const unsigned srcW = g_lastFrame.width, srcH = g_lastFrame.height;
    const float fit = std::min({1.0f, 256.0f / srcW, 192.0f / srcH});
    const unsigned dstW = std::max(1u, (unsigned)(srcW * fit));
    const unsigned dstH = std::max(1u, (unsigned)(srcH * fit));
    std::vector<uint8_t> out(dstW * dstH * 4);
    for (unsigned y = 0; y < dstH; y++)
    {
        const unsigned y0 = y * srcH / dstH, y1 = std::max(y0 + 1, (y + 1) * srcH / dstH);
        for (unsigned x = 0; x < dstW; x++)
        {
            const unsigned x0 = x * srcW / dstW, x1 = std::max(x0 + 1, (x + 1) * srcW / dstW);
            unsigned sum[3] = {0, 0, 0}, n = 0;
            for (unsigned sy = y0; sy < y1; sy++)
                for (unsigned sx = x0; sx < x1; sx++)
                {
                    uint8_t px[4];
                    FramePixel(sx, sy, px);
                    sum[0] += px[0];
                    sum[1] += px[1];
                    sum[2] += px[2];
                    n++;
                }
            uint8_t *dst = &out[(y * dstW + x) * 4];
            dst[0] = sum[0] / n;
            dst[1] = sum[1] / n;
            dst[2] = sum[2] / n;
            dst[3] = 255;
        }
    }
    struct Output {
        const std::string& path;
        bool written = false;
    } output{path};
    const auto write = [](void* context, void* bytes, int size) {
        auto& file = *static_cast<Output*>(context);
        file.written = size > 0 && TicoSafeFile::Write(file.path,bytes,static_cast<std::size_t>(size),0);
    };
    return stbi_write_png_to_func(write,&output,dstW,dstH,4,out.data(),dstW*4) && output.written;
}

static void AudioFlushCallback()
{
    g_audio.Flush();
    LOG_INFO("AUDIO", "Audio flushed");
}

bool InitImGui()
{
    LOG_INFO("HOME", "InitImGui starting...");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    if (woplay_host::active()) io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    LOG_INFO("HOME", "ImGui context created");

    // Switch has no SDL window and the overlay reads the pads itself, so the
    // SDL platform backend is desktop-only.
#ifndef __SWITCH__
    ImGui_ImplSDL2_InitForVulkan(g_window);
#ifdef WOPLAY_HOST
    if (woplay_host::active()) ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual, nullptr, 0);
#endif
#endif
    int w, h;
    GetDisplayResolution(w, h);
    if (!TicoVulkan::Init(g_window, (uint32_t)w, (uint32_t)h))
    {
        LOG_ERROR("HOME", "Vulkan initialization failed");
        return false;
    }
    LOG_INFO("HOME", "ImGui backends initialized");
    if (woplay_host::active()) return woplay_menu::loadFonts();

#ifdef __SWITCH__
    ImFontConfig fontCfg;
    fontCfg.SizePixels = TicoConfig::FONT_SIZE;
    if (io.Fonts->AddFontFromFileTTF(TicoConfig::FONT_PATH, TicoConfig::FONT_SIZE))
    {
        LOG_INFO("HOME", "Loaded ImGui font from %s", TicoConfig::FONT_PATH);
    }
    else if (!io.Fonts->AddFontDefault(&fontCfg))
    {
        LOG_ERROR("HOME", "Failed to load font from romfs and built-in ImGui fallback");
        return false;
    }
    else
    {
        LOG_WARN("HOME", "Failed to load %s, using built-in ImGui font", TicoConfig::FONT_PATH);
    }
    // Load secondary font for RA alert descriptions
    io.Fonts->AddFontFromFileTTF("romfs:/fonts/description.ttf", TicoConfig::FONT_SIZE * 0.75f);
#else
    if (!io.Fonts->AddFontFromFileTTF("tico/fonts/font.ttf", TicoConfig::FONT_SIZE))
    {
        LOG_ERROR("HOME", "Failed to load ImGui font from tico/fonts/font.ttf");
        return false;
    }
    // Load secondary font for RA alert descriptions
    io.Fonts->AddFontFromFileTTF("tico/fonts/description.ttf", TicoConfig::FONT_SIZE * 0.75f);
#endif

    LOG_INFO("HOME", "ImGui initialized");
    return true;
}

void CleanupWindow()
{
    CloseControllers();

    g_chain.reset();
    TicoSlang::Shutdown();
    TicoVulkan::Shutdown();
#ifndef __SWITCH__
    ImGui_ImplSDL2_Shutdown();
#endif
    ImGui::DestroyContext();

    if (g_window)
    {
        SDL_DestroyWindow(g_window);
    }

    SDL_Quit();
}

//==============================================================================
// Main Loop
//==============================================================================

void ProcessEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
#ifndef __SWITCH__
        bool framingInput = false;
        if (woplay_host::active()) {
            if (event.type == SDL_MOUSEMOTION)
                framingInput = woplay_menu::gamePointer({float(event.motion.x),float(event.motion.y)},
                                                        (event.motion.state & SDL_BUTTON_LMASK) != 0);
            else if ((event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) &&
                     event.button.button == SDL_BUTTON_LEFT)
                framingInput = woplay_menu::gamePointer({float(event.button.x),float(event.button.y)},
                                                        event.type == SDL_MOUSEBUTTONDOWN);
            else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                woplay_menu::gamePointer({},false);
        }
        if (!framingInput) ImGui_ImplSDL2_ProcessEvent(&event);
        // Menu shortcuts use physical X/Y on keyboard layouts that do not report Latin symbols.
        if (woplay_host::active() && (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)) {
            if(event.key.keysym.scancode==SDL_SCANCODE_X)
                ImGui::GetIO().AddKeyEvent(ImGuiKey_X,event.type==SDL_KEYDOWN);
            if(event.key.keysym.scancode==SDL_SCANCODE_Y)
                ImGui::GetIO().AddKeyEvent(ImGuiKey_Y,event.type==SDL_KEYDOWN);
            if(event.key.keysym.scancode==SDL_SCANCODE_ESCAPE)
                ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape,event.type==SDL_KEYDOWN);
        }
#endif

        if (woplay_host::active() && g_core && event.type == SDL_KEYDOWN && !event.key.repeat &&
            event.key.keysym.scancode == SDL_SCANCODE_F1 && !woplay_menu::bindingActive()) {
            if (g_menuOpen) CloseMenu();
            else OpenMenu();
        }
        if (woplay_host::active() && g_core && !g_menuOpen && event.type == SDL_KEYDOWN && !event.key.repeat) {
            using woplay_host::StateGroup;
            using woplay_host::StateOperation;
            if (event.key.keysym.scancode == SDL_SCANCODE_F5)
                RunWoPlayState({StateOperation::Save,StateGroup::Cycle});
            else if (event.key.keysym.scancode == SDL_SCANCODE_F8)
                RunWoPlayState({StateOperation::Load,StateGroup::Cycle});
        }
        if (event.type == SDL_QUIT)
        {
            LOG_INFO("HOME", "Received SDL_QUIT event");
            g_running = false;
        }

        if (event.type == SDL_CONTROLLERDEVICEADDED ||
            event.type == SDL_CONTROLLERDEVICEREMOVED ||
            event.type == SDL_JOYDEVICEADDED ||
            event.type == SDL_JOYDEVICEREMOVED)
        {
            g_controllersDirty = true;
        }

#ifdef __SWITCH__
        if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == SDL_SCANCODE_ESCAPE)
        {
            LOG_INFO("HOME", "Received Escape key event, requesting exit");
            g_running = false;
        }
#endif
    }
}

//==============================================================================
// Quick menu
//==============================================================================

static void ChainloadTico()
{
#ifdef __SWITCH__
    const char *primaryNro = "sdmc:/switch/tico.nro";
    const char *fallbackNro = "sdmc:/switch/tico/tico.nro";
    const char *targetNro = nullptr;

    struct stat buffer;
    if (stat(primaryNro, &buffer) == 0)
        targetNro = primaryNro;
    else if (stat(fallbackNro, &buffer) == 0)
        targetNro = fallbackNro;

    if (woplay_host::active()) return;
    if (targetNro != nullptr)
    {
        // Build args as space-separated string (per libnx envSetNextLoad docs)
        char args[512];
        snprintf(args, sizeof(args), "%s --resume", targetNro);
        envSetNextLoad(targetNro, args);
        LOG_INFO("HOME", "Chainloading back to %s with args: %s", targetNro, args);
    }
    else
    {
        LOG_WARN("HOME", "Chainload target not found! Exiting normally.");
    }
    remove("imgui.ini");
#endif
}

static std::string StatePath(int slot, bool loading = false)
{
    (void)loading;
    std::string romName = g_core ? g_core->GetGamePath() : std::string();
    size_t lastSlash = romName.find_last_of("/\\");
    if (lastSlash != std::string::npos)
        romName = romName.substr(lastSlash + 1);
    size_t lastDot = romName.find_last_of('.');
    if (lastDot != std::string::npos)
        romName = romName.substr(0, lastDot);
    const std::string dir = TicoConfig::StatesPath();
    TicoConfig::MakeDirs(dir);
    return dir + romName + ".state" + std::to_string(slot);
}

static int WriteWoPlayState(woplay_host::StateGroup group, int slot = 0)
{
    return woplay_host::saveState(group, slot, [](const std::string& path) {
        const auto preview = path.substr(0,path.find_last_of('/')+1) + "preview.png";
        return g_core->SaveState(path) && SaveStatePicture(preview);
    });
}

static std::string ResumeStatePath()
{
    if (!woplay_host::active()) return StatePath(OverlayUI::kAutoStateSlot - 1,true);
    try {
        return woplay_host::stateFile(woplay_host::StateGroup::Resume);
    } catch (const std::exception&) {
        return {};
    }
}

// The state the game is left in, saved to the auto slot (listed first in Load
// State) whenever the core closes: Exit, Restart, the library, HOME.
static bool AutoSaveState()
{
    if (!g_core || !g_core->IsGameLoaded() || g_core->IsHardcoreActive())
        return true;
    if (woplay_host::active()) {
        try {
            return WriteWoPlayState(woplay_host::StateGroup::Resume) > 0;
        } catch (const std::exception& error) {
            LOG_ERROR("HOME", "Save resume state: %s", error.what());
            return false;
        }
    }
    const std::string path = StatePath(OverlayUI::kAutoStateSlot - 1);
    if (!g_core->SaveState(path)) return false;
    SaveStatePicture(path + ".png");
    return true;
}

// Check the startup policy after the first game frame.
static bool g_offerResume = false;
static void OpenMenu();

static std::string CoreText(const std::string& key) {
    return woplay_host::active() ? woplay_menu::translate(key) : SwitchFrontend::OverlayTranslation::tr(key);
}

static void CoreToast(const std::string& text) {
    if (woplay_host::active()) woplay_menu::notify(text);
    else OverlayUI::ShowToast(text);
}

static void OfferResume()
{
    g_offerResume = false;
    // tico's General > Quick Menu Tip: shown once the game runs smoothly
    if (!woplay_host::active()) OverlayUI::ShowQuickMenuTip();
    const std::string mode = OverlayConfig::ResumeOnLaunch();
    if (mode == "never")
        return;
    const auto path = ResumeStatePath();
    struct stat st;
    if (!g_core || g_core->IsHardcoreActive() || path.empty() || stat(path.c_str(), &st) != 0)
        return;
    if (mode == "always")
    {
        if (g_core->LoadState(path))
            CoreToast(CoreText("emulator_auto_loaded"));
        return;
    }
    OpenMenu();
    if (g_menuOpen) {
        if (woplay_host::active()) woplay_menu::offerResume();
        else OverlayUI::ShowResumePrompt();
    }
}

//==============================================================================
// Shaders
//==============================================================================

#ifdef __SWITCH__
static const char *kBuiltinShaderDir = "romfs:/shaders/";
static const char *kUserShaderDir = "sdmc:/tico/shaders/";
#else
static const char *kBuiltinShaderDir = "tico/shaders/";
static const char *kUserShaderDir = "shaders/";
#endif

// The built-ins, with the names the menu shows for them.
static const std::pair<const char *, const char *> kBuiltinShaders[] = {
    {"lcd.slangp", "LCD"},
    {"lcd-grid-v2.slangp", "LCD Grid"},
    {"dot.slangp", "Dot Matrix"},
    {"xbrz.slangp", "xBRZ"},
    {"eagle.slangp", "Eagle"},
    {"crt-easymode.slangp", "CRT Easy Mode"},
};

static bool EndsWith(const std::string &s, const char *suffix)
{
    const size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

static std::string ShaderPreset()
{
    if (woplay_host::active()) {
        const auto text = woplay_host::optionJson("shader_preset");
        const auto value = nlohmann::json::parse(text, nullptr, false);
        return value.is_string() ? value.get<std::string>() : std::string{};
    }
    return OverlayConfig::GetConfigValue("shader_preset", "");
}

static void SetShaderPreset(const std::string &path)
{
    if (woplay_host::active()) {
        woplay_host::setOptionJson("shader_preset", nlohmann::json(path).dump());
        return;
    }
    OverlayConfig::SetConfigValue("shader_preset", path);
    OverlayConfig::SaveConfig();
}

// Settings from before slang presets picked one of the old GL shaders.
static void MigrateShaderSetting()
{
    if (OverlayConfig::GetConfigValue("shader_preset", "\x01") != "\x01")
        return; // already chosen, "" included
    static const std::pair<const char *, const char *> kOld[] = {
        {"LCD", "lcd.slangp"}, {"xBRZ", "xbrz.slangp"}, {"Eagle", "eagle.slangp"},
        {"Dot", "dot.slangp"}, {"LcdGridV2", "lcd-grid-v2.slangp"},
    };
    const std::string old = OverlayConfig::GetConfigValue("shader_type", "None");
    for (const auto &entry : kOld)
        if (old == entry.first)
            SetShaderPreset(kBuiltinShaderDir + std::string(entry.second));
}

static std::string ShaderPresetLabel()
{
    const std::string preset = ShaderPreset();
    if (preset.empty())
        return std::string();
    for (const auto &builtin : kBuiltinShaders)
        if (preset == kBuiltinShaderDir + std::string(builtin.first))
            return builtin.second;
    std::string name = preset;
    const size_t slash = name.find_last_of('/');
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    return EndsWith(name, ".slangp") ? name.substr(0, name.size() - 7) : name;
}

// The browser: the user folder lists the built-ins first, every other folder
// its parent; then subfolders and presets, by name.
static std::vector<OverlayUI::ShaderBrowseEntry> BrowseShaders(std::string dir)
{
    using Entry = OverlayUI::ShaderBrowseEntry;
    if (dir.empty() || dir.back() != '/')
        dir += '/';
    std::vector<Entry> entries;
    if (dir == kUserShaderDir)
    {
        entries.push_back({"> " + SwitchFrontend::OverlayTranslation::tr("emulator_builtin_shaders"),
                           kBuiltinShaderDir, true});
        entries.push_back({SwitchFrontend::OverlayTranslation::tr("emulator_none"), "", false});
    }
    else
    {
        std::string parent = kUserShaderDir;
        if (dir != kBuiltinShaderDir)
        {
            const std::string d = dir.substr(0, dir.size() - 1);
            const size_t slash = d.find_last_of('/');
            if (slash != std::string::npos)
                parent = d.substr(0, slash + 1);
        }
        entries.push_back({"..", parent, true});
    }
    if (dir == kBuiltinShaderDir)
    {
        for (const auto &builtin : kBuiltinShaders)
            entries.push_back({builtin.second, dir + builtin.first, false});
        return entries;
    }

    std::vector<Entry> dirs, files;
    if (DIR *d = opendir(dir.c_str()))
    {
        while (struct dirent *e = readdir(d))
        {
            const std::string name = e->d_name;
            if (name.empty() || name[0] == '.')
                continue;
            const std::string path = dir + name;
            bool isDir = e->d_type == DT_DIR;
            if (e->d_type == DT_UNKNOWN)
            {
                struct stat st;
                isDir = stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
            }
            if (isDir)
                dirs.push_back({name + "/", path + "/", true});
            else if (EndsWith(name, ".slangp"))
                files.push_back({name.substr(0, name.size() - 7), path, false});
        }
        closedir(d);
    }
    auto byName = [](const Entry &a, const Entry &b) {
        return strcasecmp(a.label.c_str(), b.label.c_str()) < 0;
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    entries.insert(entries.end(), dirs.begin(), dirs.end());
    entries.insert(entries.end(), files.begin(), files.end());
    return entries;
}

// Parameter overrides per preset, in mgba.jsonc's shader_parameters.
static nlohmann::json ShaderParameterOverrides()
{
    const std::string text = OverlayConfig::GetConfigJson("shader_parameters");
    nlohmann::json j = text.empty() ? nlohmann::json::object()
                                    : nlohmann::json::parse(text, nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}

static void SaveShaderParameterOverrides(const nlohmann::json &j)
{
    OverlayConfig::SetConfigJson("shader_parameters", j.dump());
    OverlayConfig::SaveConfig();
}

// A preset just loaded: start from its defaults, then the saved overrides.
static void OnShaderLoaded()
{
    if (!g_chain)
        return;
    g_chain->ResetParameters();
    const nlohmann::json overrides = ShaderParameterOverrides();
    const auto it = overrides.find(woplay_host::active() ? woplay_host::shaderPreferenceKey() : ShaderPreset());
    if (it == overrides.end() || !it->is_object())
        return;
    for (const auto &param : it->items())
        if (param.value().is_number())
            g_chain->SetParameter(param.key(), param.value().get<float>());
}

static void SetShaderParameter(const std::string &id, float value)
{
    if (!g_chain)
        return;
    g_chain->SetParameter(id, value);
    nlohmann::json overrides = ShaderParameterOverrides();
    const auto key = woplay_host::active() ? woplay_host::shaderPreferenceKey() : ShaderPreset();
    nlohmann::json &preset = overrides[key];
    if (!preset.is_object())
        preset = nlohmann::json::object();
    for (const TicoSlang::Parameter &p : g_chain->Parameters())
    {
        if (p.id != id)
            continue;
        const float step = p.step > 0.0f ? p.step : 0.01f;
        if (std::fabs(value - p.initial) < step * 0.5f)
            preset.erase(id);
        else
            preset[id] = value;
    }
    if (preset.empty())
        overrides.erase(key);
    SaveShaderParameterOverrides(overrides);
}

static void ResetShaderParameters()
{
    if (g_chain)
        g_chain->ResetParameters();
    nlohmann::json overrides = ShaderParameterOverrides();
    overrides.erase(woplay_host::active() ? woplay_host::shaderPreferenceKey() : ShaderPreset());
    SaveShaderParameterOverrides(overrides);
}

static void RegisterShaderMenu()
{
    OverlayUI::ShaderCallbacks callbacks;
    callbacks.preset_label = [] { return ShaderPresetLabel(); };
    callbacks.browse_start = [] {
        const std::string preset = ShaderPreset();
        const size_t slash = preset.find_last_of('/');
        return slash == std::string::npos ? std::string(kUserShaderDir) : preset.substr(0, slash + 1);
    };
    callbacks.browse = [](const std::string &dir) { return BrowseShaders(dir); };
    callbacks.select = [](const std::string &path) { SetShaderPreset(path); };
    callbacks.parameters = [] {
        std::vector<OverlayUI::ShaderParameter> out;
        if (g_chain)
            for (const TicoSlang::Parameter &p : g_chain->Parameters())
                out.push_back({p.id, p.description, p.value, p.minimum, p.maximum, p.step});
        return out;
    };
    callbacks.set_parameter = [](const std::string &id, float value) { SetShaderParameter(id, value); };
    callbacks.reset_parameters = [] { ResetShaderParameters(); };
    if (woplay_host::active()) woplay_menu::shaders(std::move(callbacks));
    else OverlayUI::SetShaderCallbacks(std::move(callbacks));
}

// Loads the preset the settings name once it differs from the active one.
// Compiling can take a while on the Switch, so the frame before it shows a
// toast instead of the screen just freezing.
static void ApplyShaderPreset()
{
    const std::string wanted = woplay_host::shaderEnabled() ? ShaderPreset() : std::string{};
    if (!g_chain)
        return;
    static std::uint64_t appliedRevision = 0;
    static std::string failedPreset;
    static std::uint64_t failedRevision = 0;
    const auto revision = woplay_host::displayConfigRevision();
    if (wanted == g_activePreset) {
        if (woplay_host::active() && revision != appliedRevision) {
            appliedRevision = revision;
            OnShaderLoaded();
        }
        return;
    }
    if (woplay_host::active() && wanted == failedPreset && revision == failedRevision) return;
    static std::string announced;
    if (announced != wanted && !wanted.empty())
    {
        announced = wanted;
        CoreToast(CoreText("emulator_loading_shader"));
        return;
    }
    announced.clear();
    std::string error;
    if (g_chain->LoadPreset(wanted, error))
    {
        g_activePreset = wanted;
        appliedRevision = revision;
        OnShaderLoaded();
        return;
    }
    LOG_ERROR("SHADER", "Cannot load %s: %s", wanted.c_str(), error.c_str());
    const std::string firstLine = error.substr(0, error.find('\n'));
    CoreToast(CoreText("emulator_shader_failed") + ": " + firstLine.substr(0, 80));
    if (woplay_host::active()) {
        failedPreset = wanted;
        failedRevision = revision;
        return;
    }
    // Keep showing (and saving) what actually runs.
    if (g_activePreset == "\x01")
        g_activePreset.clear();
    SetShaderPreset(g_activePreset);
}

// settings.json is the one settings definition: every core option it lists
// reaches the core, with its default when the config file does not set it.
static void ApplySettingsToCore()
{
    if (!g_core)
        return;
    if (woplay_host::active()) {
        for (const auto& [key, value] : woplay_host::nativeOptions(g_core->IsGameLoaded()))
            g_core->SetOption(key, value);
        return;
    }
    OverlayConfig::ApplyToCore([](const std::string &key, const std::string &value) {
        g_core->SetOption(key, value);
    });
}

static std::string TrFormat(const char *key, int value)
{
    SwitchFrontend::OverlayTranslation::TranslationManager::Instance().Init();
    const std::string format = CoreText(key);
    char text[256];
    snprintf(text, sizeof(text), format.c_str(), value);
    return text;
}

static void OpenMenu()
{
    if (!g_overlayReady || g_menuOpen)
        return;
    // Opening the menu pauses the game, which hardcore only allows so often.
    int waitSeconds = 0;
    if (g_core && !g_core->CanPause(waitSeconds))
    {
        OverlayUI::ShowToast(TrFormat("emulator_hardcore_pause_wait", waitSeconds));
        return;
    }
    TicoCore::StopRumble(); // the game is paused
    if (woplay_host::active()) woplay_host::pause();
    g_menuOpen = true;
    g_navHeldPrev = 0;
    g_navRepeatFrames = 0;
    OverlayUI::SetHardcoreMode(g_core && g_core->IsHardcoreActive());
    StopFastForward();
    if (woplay_host::active()) woplay_menu::visible(true);
    else ImGuiOverlay::SetVisible(true);
}

static void CloseMenu()
{
    if (!g_menuOpen)
        return;
    g_menuOpen = false;
    if (woplay_host::active()) {
        g_waitGameInput = true;
        woplay_menu::visible(false);
        woplay_menu::input(nullptr);
    } else ImGuiOverlay::SetVisible(false);
    if (g_core)
        g_core->ClearInputs();
}

// D-pad + left stick, edge plus hold-repeat; Switch A accepts, B goes back.
// The first finger on the touchscreen, for the menu (no controller needed).
static void FeedMenuTouch()
{
    OverlayUI::TouchInput touch{};
#ifdef __SWITCH__
    static bool initialized = false;
    if (!initialized)
    {
        hidInitializeTouchScreen();
        initialized = true;
    }
    HidTouchScreenState state{};
    if (hidGetTouchScreenStates(&state, 1) > 0 && state.count > 0)
        touch = {true, static_cast<float>(state.touches[0].x), static_cast<float>(state.touches[0].y)};
#endif
    if (woplay_host::active()) {
#ifdef __SWITCH__
        woplay_menu::touch(touch);
#endif
    } else ImGuiOverlay::FeedTouch(touch);
}

static void FeedMenu(SDL_GameController *pad)
{
    enum : uint32_t { Up = 1, Down = 2, Left = 4, Right = 8 };
    const Sint16 axisX = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    const Sint16 axisY = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    uint32_t held = 0;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) || axisY < -16000) held |= Up;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || axisY > 16000) held |= Down;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || axisX < -16000) held |= Left;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || axisX > 16000) held |= Right;

    uint32_t fire = held & ~g_navHeldPrev; // new presses fire instantly
    if (held != 0 && held == g_navHeldPrev)
    {
        if (--g_navRepeatFrames <= 0)
        {
            fire |= held;
            g_navRepeatFrames = kNavRepeatFrames;
        }
    }
    else if (fire != 0)
    {
        g_navRepeatFrames = kNavInitialDelayFrames;
    }
    g_navHeldPrev = held;

    // SDL names buttons by position: B is the Switch A (east), A the Switch B.
    static bool acceptHeld = false;
    static bool cancelHeld = false;
    const bool accept = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B);
    const bool cancel = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A);
    ImGuiOverlay::FeedNav({
        .up = (fire & Up) != 0,
        .down = (fire & Down) != 0,
        .left = (fire & Left) != 0,
        .right = (fire & Right) != 0,
        .accept = accept && !acceptHeld,
        .cancel = cancel && !cancelHeld,
    });
    acceptHeld = accept;
    cancelHeld = cancel;
}

static void RunWoPlayState(const woplay_host::StateRequest& request)
{
    using woplay_host::StateGroup;
    using woplay_host::StateOperation;
    if (!g_core) return;
    try {
        switch (request.operation) {
        case StateOperation::Save: {
            const int slot = WriteWoPlayState(request.group,request.slot);
            CoreToast(TrFormat("已保存到槽位 %d",slot));
            break;
        }
        case StateOperation::Load: {
            if (g_core->IsHardcoreActive()) {
                CoreToast(CoreText("emulator_hardcore_no_load"));
                return;
            }
            const auto path = woplay_host::stateFile(request.group,request.slot);
            if (path.empty()) {
                CoreToast(CoreText(request.group == StateGroup::Cycle ? "没有循环存档" : "空槽位"));
                return;
            }
            if (!g_core->LoadState(path)) {
                CoreToast(CoreText("读取存档失败"));
                return;
            }
            CoreToast(CoreText(request.group == StateGroup::Resume ? "emulator_auto_loaded" : "已读取存档"));
            CloseMenu();
            break;
        }
        case StateOperation::Delete:
            woplay_host::deleteState(request.group,request.slot);
            CoreToast(CoreText("存档已删除"));
            break;
        case StateOperation::Expand:
            woplay_host::expandStates(request.group);
            CoreToast(CoreText("已扩充档位"));
            break;
        }
    } catch (const std::exception& error) {
        LOG_ERROR("HOME", "Save state operation failed: %s",error.what());
        CoreToast(CoreText(request.operation == StateOperation::Load ?
                          "读取存档失败" : "保存失败，请重试"));
    }
}

// Carries out what the menu chose on the last drawn frame.
static void RunMenuAction()
{
    using OverlayUI::Action;
    if (const auto request = woplay_menu::takeStateAction()) RunWoPlayState(*request);
    const Action action = woplay_host::active() ? woplay_menu::takeAction() : ImGuiOverlay::ConsumeAction();
    if (!woplay_host::active() && OverlayUI::ConsumeSettingsChanged())
        ApplySettingsToCore();

    switch (action)
    {
    case Action::None:
        return;
    case Action::Resume:
        CloseMenu();
        return;
    case Action::Exit:
        LOG_INFO("HOME", "Exit requested");
        if (g_standalone)
        {
            // a game returns to the library; the library itself quits
            if (g_core)
                ShowLibrary();
            else
                g_running = false;
            return;
        }
        if (woplay_host::active() && g_core) {
            if (!g_core->SavePersistent() || !AutoSaveState()) {
                CoreToast(CoreText("保存失败，请重试"));
                return;
            }
            g_woplayAutoSaved = true;
        }
        CloseMenu();
        ChainloadTico();
        g_running = false;
        return;
    case Action::Reset:
        if (g_core)
            g_core->Reset();
        CloseMenu();
        return;
    case Action::Restart:
        // Load the game again from disk, as if it were started anew: the core
        // unloads first (saving the game), so two never run at once.
        if (g_core)
        {
            const std::string path = g_core->GetGamePath();
            const std::string slug = TicoConfig::CURRENT_SLUG;
            const std::string title = g_titleArg;
            if (woplay_host::active()) {
                if (!g_core->SavePersistent() || !AutoSaveState()) {
                    CoreToast(CoreText("保存失败，请重试"));
                    return;
                }
            } else AutoSaveState();
            CloseMenu();
            TicoVulkan::WaitIdle();
            g_core.reset();
            TicoCore::StopRumble();
            StopFastForward();
            AudioFlushCallback(); // nothing of the old session plays into the new one
            StartGame(slug, path, title);
            g_offerResume = false; // Restart means from the start
            if (woplay_host::active() && (!g_core || !g_core->IsGameLoaded())) {
                woplay_host::finish("ROM restart failed");
                g_running = false;
            }
        }
        return;
    default:
        break;
    }

    if (OverlayUI::IsSaveStateAction(action) && g_core)
    {
        const int slot = OverlayUI::GetStateSlotForAction(action);
        const bool saved = g_core->SaveState(StatePath(slot - 1));
        if (saved)
            SaveStatePicture(StatePath(slot - 1) + ".png");
        CoreToast(TrFormat(saved ? "emulator_state_saved" : "emulator_save_failed", slot));
        if (!woplay_host::active()) CloseMenu();
    }
    else if (OverlayUI::IsLoadStateAction(action) && g_core)
    {
        const int slot = OverlayUI::GetStateSlotForAction(action);
        if (g_core->IsHardcoreActive())
            OverlayUI::ShowToast(SwitchFrontend::OverlayTranslation::tr("emulator_hardcore_no_load"));
        else
        {
            const bool loaded = g_core->LoadState(StatePath(slot - 1, true));
            if (loaded && slot == OverlayUI::kAutoStateSlot)
                CoreToast(CoreText("emulator_auto_loaded"));
            else
                CoreToast(TrFormat(loaded ? "emulator_state_loaded" : "emulator_load_failed", slot));
            if (woplay_host::active() && !loaded) return;
        }
        CloseMenu();
    }
}

static void UpdateHud(float deltaTime)
{
    g_hudFrames++;
    g_hudSeconds += deltaTime;
    if (g_hudSeconds >= 0.5f)
    {
        g_hudFps = static_cast<float>(g_hudFrames) / g_hudSeconds;
        g_hudFrames = 0;
        g_hudSeconds = 0.0f;
    }
    OverlayUI::HudStats stats;
    stats.fps = g_hudFps;
    stats.fast_forward = g_audio.IsFastForwarding();
    if (g_core)
    {
        stats.rendered_width = g_core->GetFrameWidth();
        stats.rendered_height = g_core->GetFrameHeight();
    }
    if (woplay_host::active()) {
        if (!g_menuOpen && g_core) woplay_menu::hud(stats.fps, stats.rendered_width, stats.rendered_height, stats.fast_forward);
    } else OverlayUI::SetHudStats(stats);
}

// The game's on-screen rectangle, from the Display tab: Integer scales the
// frame by 1x, 2x or the largest that fits ("Auto"); Display fits an aspect
// ratio (4:3, 16:9, the core's own "Original") or stretches.
static ImVec4 ComputeGameRect(ImVec2 displaySize)
{
    const int width = g_core ? g_core->GetFrameWidth() : 240;
    const int height = g_core ? g_core->GetFrameHeight() : 160;
    const float aspectRatio = g_core ? g_core->GetAspectRatio() : 4.0f / 3.0f;
    const std::string mode = OverlayConfig::GetConfigValue("display_mode", "Integer");
    const std::string size = OverlayConfig::GetConfigValue("display_size", "Auto");

    const float baseW = width > 0 ? static_cast<float>(width) : 240.0f;
    const float baseH = height > 0 ? static_cast<float>(height) : 160.0f;
    float dstWidth = displaySize.x;
    float dstHeight = displaySize.y;
    if (woplay_host::active()) {
        const auto geometry = woplay_host::displayGeometry();
        const auto fit = [&](float ar) {
            dstWidth = std::min(displaySize.x,displaySize.y*ar);
            dstHeight = dstWidth/ar;
        };
        float x = 0, y = 0;
        if (geometry.mode == "integer") {
            const auto maxScale = std::min(displaySize.x/baseW,displaySize.y/baseH);
            const auto ratio = geometry.integer ? float(geometry.integer) : std::max(1.F,std::floor(maxScale));
            dstWidth = baseW*std::min(ratio,maxScale);
            dstHeight = baseH*std::min(ratio,maxScale);
        } else if (geometry.mode == "original") {
            dstWidth = baseW;
            dstHeight = baseH;
        } else if (geometry.mode != "fill") {
            fit(geometry.mode == "four_three" ? 4.F/3 : geometry.mode == "wide" ? 16.F/9 :
                aspectRatio > 0.F ? aspectRatio : baseW/baseH);
            if (geometry.mode == "custom") {
                const auto& frame = geometry.customFrame;
                if (frame[2] > 0.F && frame[3] > 0.F)
                    return {frame[0]*displaySize.x,frame[1]*displaySize.y,
                            frame[2]*displaySize.x,frame[3]*displaySize.y};
                dstWidth *= geometry.scale;
                dstHeight *= geometry.scale;
                x = geometry.offsetX*displaySize.x/1280.F;
                y = geometry.offsetY*displaySize.y/720.F;
            }
        }
        return {std::floor((displaySize.x-dstWidth)*.5F+x),std::floor((displaySize.y-dstHeight)*.5F+y),
                std::floor(dstWidth),std::floor(dstHeight)};
    }
    if (mode == "Integer")
    {
        int scale;
        if (size == "1x")
            scale = 1;
        else if (size == "2x")
            scale = 2;
        else
            scale = std::max(1, std::min(static_cast<int>(displaySize.x / baseW),
                                         static_cast<int>(displaySize.y / baseH)));
        dstWidth = std::min(displaySize.x, baseW * scale);
        dstHeight = std::min(displaySize.y, baseH * scale);
    }
    else if (size != "Stretch")
    {
        float ar = aspectRatio > 0.0f ? aspectRatio : baseW / baseH;
        if (size == "4:3")
            ar = 4.0f / 3.0f;
        else if (size == "16:9")
            ar = 16.0f / 9.0f;
        if (ar > displaySize.x / displaySize.y)
        {
            dstWidth = displaySize.x;
            dstHeight = displaySize.x / ar;
        }
        else
        {
            dstHeight = displaySize.y;
            dstWidth = displaySize.y * ar;
        }
    }
    dstWidth = std::floor(dstWidth);
    dstHeight = std::floor(dstHeight);
    return ImVec4(std::floor((displaySize.x - dstWidth) / 2.0f),
                  std::floor((displaySize.y - dstHeight) / 2.0f), dstWidth, dstHeight);
}

// Runs the shader chain at the game's on-screen size and draws its output.
static ImVec4 DrawGame(VkCommandBuffer cmd, ImDrawList *dl, ImVec2 displaySize)
{
    dl->AddRectFilled(ImVec2(0, 0), displaySize, IM_COL32(0, 0, 0, 255));
    if (!g_core) return {}; // no stale frame in the library
    const ImVec4 configured = ComputeGameRect(displaySize);
    const ImVec4 rect = woplay_menu::previewFrame(displaySize).value_or(configured);
    if (!g_chain || !cmd || rect.z < 1.0f || rect.w < 1.0f) return rect;
    const float ar = g_core->GetAspectRatio();
    // Resizing only scales the displayed quad. Rebuild the shader targets after confirmation.
    const auto outputW = std::clamp(configured.z,1.F,displaySize.x);
    const auto outputH = std::clamp(configured.w,1.F,displaySize.y);
    const ImTextureID tex = g_chain->Process(cmd, (uint32_t)outputW, (uint32_t)outputH, ar,
                                             g_core ? g_core->GetFPS() : 60.0);
    if (tex != ImTextureID_Invalid)
        dl->AddImage(tex, ImVec2(rect.x, rect.y), ImVec2(rect.x + rect.z, rect.y + rect.w));
    if (woplay_host::active()) woplay_menu::drawOverlay(*dl,displaySize);
    return rect;
}

// Switch buttons by their Nintendo names, as the Controls tab spells them.
enum class SwitchButton
{
    A, B, X, Y, L, R, ZL, ZR, Plus, Minus, StickL, StickR, Up, Down, Left, Right,
    LStickUp, LStickDown, LStickLeft, LStickRight, RStickUp, RStickDown, RStickLeft, RStickRight, Count
};

static constexpr uint32_t SwitchBit(SwitchButton button)
{
    return 1u << static_cast<unsigned>(button);
}

static uint32_t SwitchBitFor(std::string_view name)
{
    static const std::pair<const char *, SwitchButton> kNames[] = {
        {"A", SwitchButton::A}, {"B", SwitchButton::B}, {"X", SwitchButton::X},
        {"Y", SwitchButton::Y}, {"L", SwitchButton::L}, {"R", SwitchButton::R},
        {"ZL", SwitchButton::ZL}, {"ZR", SwitchButton::ZR}, {"Plus", SwitchButton::Plus},
        {"Minus", SwitchButton::Minus}, {"StickL", SwitchButton::StickL},
        {"StickR", SwitchButton::StickR}, {"Up", SwitchButton::Up},
        {"Down", SwitchButton::Down}, {"Left", SwitchButton::Left},
        {"Right", SwitchButton::Right},
        {"LStickUp",SwitchButton::LStickUp},{"LStickDown",SwitchButton::LStickDown},
        {"LStickLeft",SwitchButton::LStickLeft},{"LStickRight",SwitchButton::LStickRight},
        {"RStickUp",SwitchButton::RStickUp},{"RStickDown",SwitchButton::RStickDown},
        {"RStickLeft",SwitchButton::RStickLeft},{"RStickRight",SwitchButton::RStickRight},
    };
    for (const auto &entry : kNames)
        if (name == entry.first)
            return SwitchBit(entry.second);
    return 0; // "None"
}

// SDL names buttons by position (Xbox layout): its B is the Switch A, its A
// the Switch B, its Y the Switch X and its X the Switch Y.
static uint32_t SwitchButtonsHeld(SDL_GameController *pad)
{
    struct SdlButton
    {
        SDL_GameControllerButton sdl;
        SwitchButton button;
    };
    static const SdlButton kButtons[] = {
        {SDL_CONTROLLER_BUTTON_B, SwitchButton::A},
        {SDL_CONTROLLER_BUTTON_A, SwitchButton::B},
        {SDL_CONTROLLER_BUTTON_Y, SwitchButton::X},
        {SDL_CONTROLLER_BUTTON_X, SwitchButton::Y},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SwitchButton::L},
        {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SwitchButton::R},
        {SDL_CONTROLLER_BUTTON_START, SwitchButton::Plus},
        {SDL_CONTROLLER_BUTTON_BACK, SwitchButton::Minus},
        {SDL_CONTROLLER_BUTTON_LEFTSTICK, SwitchButton::StickL},
        {SDL_CONTROLLER_BUTTON_RIGHTSTICK, SwitchButton::StickR},
        {SDL_CONTROLLER_BUTTON_DPAD_UP, SwitchButton::Up},
        {SDL_CONTROLLER_BUTTON_DPAD_DOWN, SwitchButton::Down},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, SwitchButton::Left},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SwitchButton::Right},
    };
    uint32_t held = 0;
    for (const SdlButton &button : kButtons)
        if (SDL_GameControllerGetButton(pad, button.sdl))
            held |= SwitchBit(button.button);
    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000)
        held |= SwitchBit(SwitchButton::ZL);
    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000)
        held |= SwitchBit(SwitchButton::ZR);
    const auto stick=[&](SDL_GameControllerAxis x,SDL_GameControllerAxis y,SwitchButton up,SwitchButton down,
                         SwitchButton left,SwitchButton right){
        const auto horizontal=SDL_GameControllerGetAxis(pad,x),vertical=SDL_GameControllerGetAxis(pad,y);
        if(vertical<-16000)held|=SwitchBit(up);
        if(vertical>16000)held|=SwitchBit(down);
        if(horizontal<-16000)held|=SwitchBit(left);
        if(horizontal>16000)held|=SwitchBit(right);
    };
    stick(SDL_CONTROLLER_AXIS_LEFTX,SDL_CONTROLLER_AXIS_LEFTY,SwitchButton::LStickUp,SwitchButton::LStickDown,
          SwitchButton::LStickLeft,SwitchButton::LStickRight);
    stick(SDL_CONTROLLER_AXIS_RIGHTX,SDL_CONTROLLER_AXIS_RIGHTY,SwitchButton::RStickUp,SwitchButton::RStickDown,
          SwitchButton::RStickLeft,SwitchButton::RStickRight);
    return held;
}

static bool MappingHeld(const std::string& value,uint32_t held,bool keyboard,uint32_t* used=nullptr)
{
#ifdef WOPLAY_HOST
    if(woplay_host::active()){
        uint32_t mask=0;
        const bool down=gbastation::ui::bindingHeld(value,[&](std::string_view token){
            const auto button=SwitchBitFor(token);
            mask|=button;
            return button ? (held&button)!=0 : keyboard&&gbastation::ui::keyboardBindingDown(token);
        });
        if(used)*used=down?mask:0;
        return down;
    }
#endif
    const auto bit=SwitchBitFor(value);
    if(used)*used=bit;
    return (held&bit)!=0;
}

// GBA (and Game Boy) buttons, with the Switch button each sits on by default:
// each on its namesake, Start and Select on Plus and Minus.
struct ButtonMapping
{
    const char *key;
    const char *fallback;
    unsigned retroId;
};
static const ButtonMapping kButtonMappings[] = {
    {"map_a", "A", RETRO_DEVICE_ID_JOYPAD_A},
    {"map_b", "B", RETRO_DEVICE_ID_JOYPAD_B},
    {"map_l", "L", RETRO_DEVICE_ID_JOYPAD_L},
    {"map_r", "R", RETRO_DEVICE_ID_JOYPAD_R},
    {"map_start", "Plus", RETRO_DEVICE_ID_JOYPAD_START},
    {"map_select", "Minus", RETRO_DEVICE_ID_JOYPAD_SELECT},
    {"map_up", "Up", RETRO_DEVICE_ID_JOYPAD_UP},
    {"map_down", "Down", RETRO_DEVICE_ID_JOYPAD_DOWN},
    {"map_left", "Left", RETRO_DEVICE_ID_JOYPAD_LEFT},
    {"map_right", "Right", RETRO_DEVICE_ID_JOYPAD_RIGHT},
};

// Updates fast forward from player 1's hotkey and returns the hotkey's
// Switch button (0 when there is none), which then stays out of the game.
static uint32_t UpdateFastForward(SDL_GameController *pad)
{
    uint32_t button=0;
    const bool down=MappingHeld(OverlayConfig::GetConfigValue("fast_forward_hotkey","ZR"),
                                pad?SwitchButtonsHeld(pad):0,true,&button);
    bool active;
    if (OverlayConfig::GetConfigValue("fast_forward_mode", "hold") == "toggle")
    {
        if (down && !g_ffHotkeyHeld)
            g_ffLatched = !g_ffLatched;
        active = g_ffLatched;
    }
    else
    {
        active = down;
    }
    g_ffHotkeyHeld = down;
    if (!active)
        g_ffFrameBudget = 0.0f;
    g_audio.SetFastForward(active);
    return button;
}

static void StopFastForward()
{
    g_ffLatched = false;
    g_ffFrameBudget = 0.0f;
    g_audio.SetFastForward(false);
}

// Core frames to run before the next present.
static int FramesThisRefresh()
{
    if (!g_audio.IsFastForwarding())
        return 1;
    const std::string speed = OverlayConfig::GetConfigValue("fast_forward_speed", "200");
    if (speed == "unlimited")
        return 1; // vsync is off instead
    float rate = std::max(1.0f, std::atoi(speed.c_str()) / 100.0f);
    g_ffFrameBudget += rate;
    const int frames = static_cast<int>(g_ffFrameBudget);
    g_ffFrameBudget -= frames;
    return std::max(1, frames);
}

// The display refreshes at 60 Hz and vsync paces the loop, so the core's own
// frame rate has to be mapped onto that. Content near 60 Hz (NTSC, 60.10 fps)
// runs one core frame per vsync, with audio stretched to absorb the small
// difference. Anything else (PAL, 50.007 fps) is paced by an accumulator so it
// runs at real speed instead of 60/50 = 120%.
static constexpr double DISPLAY_HZ = 60.0;
static double g_pacedFps = 0.0;
static double g_frameStep = 1.0;
static double g_frameAccum = 0.0;

static void UpdateFramePacing()
{
    double fps = g_core->GetFPS();
    if (fps <= 0.0 || fps == g_pacedFps)
        return;

    g_pacedFps = fps;
    double effectiveFps = std::fabs(fps - DISPLAY_HZ) < 1.0 ? DISPLAY_HZ : fps;
    g_frameStep = effectiveFps / DISPLAY_HZ;
    g_frameAccum = 0.0;

    g_audio.SetCoreSampleRate(g_core->GetSampleRate() * (effectiveFps / fps));
    LOG_INFO("AUDIO", "Core %.3f fps, %.0f Hz: running %.3f core frames per vsync",
             fps, g_core->GetSampleRate(), g_frameStep);
}

static bool FastForwardUncapped()
{
    return g_audio.IsFastForwarding() &&
           OverlayConfig::GetConfigValue("fast_forward_speed", "200") == "unlimited";
}

//==============================================================================
// Library (standalone launch)
//==============================================================================

static const char *kRomExtensions[] = {".gba", ".gb", ".gbc", ".sgb", ".zip", ".7z", ".rar"};

static std::string LowerExtension(const std::string &path)
{
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return std::string();
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}

// mGBA's module serves one console slug, gba, for GBA, GB and GBC games alike.
static std::string SlugForRom(const std::string &)
{
    return "gba";
}

// The consoles the library lists, each with its own folders.
struct LibraryConsole
{
    const char *slug;
    const char *title;
};
static const LibraryConsole kLibraryConsoles[] = {
    {"gba", "Game Boy Advance"},
};

static std::string WithSlash(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    if (!path.empty() && path.back() != '/')
        path += '/';
    return path;
}

// tico's ROM bases (general.jsonc): the ROMs path, then the extra bases. A
// console's games are in <base>/<slug>/ under each, as tico scans them.
static std::vector<std::string> TicoRomBases()
{
    std::vector<std::string> bases;
    tico::SettingsStream file("general");
    const nlohmann::json j = file.good() ? nlohmann::json::parse(file, nullptr, false, true)
                                         : nlohmann::json();
    std::string roms = j.is_object() ? j.value("roms_path", std::string()) : std::string();
    bases.push_back(WithSlash(roms.empty() ? "sdmc:/tico/roms/" : roms));
    if (j.is_object() && j.contains("rom_base_paths") && j["rom_base_paths"].is_array())
        for (const auto &base : j["rom_base_paths"])
            if (base.is_string() && !base.get<std::string>().empty())
                bases.push_back(WithSlash(base.get<std::string>()));
    return bases;
}

// The module's own folders per console (tico_rom_folders in mgba.jsonc),
// the same list tico's Paths tab edits.
static nlohmann::json ModuleRomFolders()
{
    const std::string text = OverlayConfig::GetConfigJson("tico_rom_folders");
    nlohmann::json j = text.empty() ? nlohmann::json::object()
                                    : nlohmann::json::parse(text, nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}

static std::vector<std::string> ModuleRomFolders(const std::string &slug)
{
    std::vector<std::string> folders;
    const nlohmann::json all = ModuleRomFolders();
    const auto it = all.find(slug);
    if (it != all.end() && it->is_array())
        for (const auto &entry : *it)
            if (entry.is_string() && !entry.get<std::string>().empty())
                folders.push_back(WithSlash(entry.get<std::string>()));
    return folders;
}

static void SetModuleRomFolders(const std::string &slug, const std::vector<std::string> &folders)
{
    nlohmann::json all = ModuleRomFolders();
    if (folders.empty())
        all.erase(slug);
    else
        all[slug] = folders;
    OverlayConfig::SetConfigJson("tico_rom_folders", all.dump());
    OverlayConfig::SaveConfig();
}

// Every folder a console's games are read from: each base's <base>/<slug>/,
// then the module's own folders.
static std::vector<std::string> RomFoldersFor(const std::string &slug)
{
    std::vector<std::string> folders;
    auto add = [&](const std::string &folder) {
        // a folder on a USB drive is read through the drive's current mount,
        // and left out while the drive is not connected
        const std::string mounted = UsbStorage::Resolve(folder);
        if (!mounted.empty() && std::find(folders.begin(), folders.end(), mounted) == folders.end())
            folders.push_back(mounted);
    };
    for (const std::string &base : TicoRomBases())
        add(base + slug + "/");
    for (const std::string &folder : ModuleRomFolders(slug))
        add(folder);
    return folders;
}

// The console of each listed game, by path: the folder list it was found in.
static std::map<std::string, std::string> g_librarySlugs;

static void ScanRomFolder(const std::string &dir, int depth, std::vector<std::string> &out)
{
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    while (struct dirent *e = readdir(d))
    {
        const std::string name = e->d_name;
        if (name.empty() || name[0] == '.')
            continue;
        const std::string path = (dir.back() == '/' ? dir : dir + "/") + name;
        bool isDir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN)
        {
            struct stat st;
            isDir = stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (isDir)
        {
            if (depth > 0)
                ScanRomFolder(path, depth - 1, out);
            continue;
        }
        const std::string ext = LowerExtension(name);
        for (const char *known : kRomExtensions)
            if (ext == known)
                out.push_back(path);
    }
    closedir(d);
}

static std::vector<OverlayUI::LibraryEntry> ListLibrary()
{
    g_librarySlugs.clear();
    std::vector<OverlayUI::LibraryEntry> entries;
    for (const LibraryConsole &console : kLibraryConsoles)
    {
        std::vector<std::string> roms;
        for (const std::string &folder : RomFoldersFor(console.slug))
            ScanRomFolder(folder, 2, roms);
        std::sort(roms.begin(), roms.end());
        roms.erase(std::unique(roms.begin(), roms.end()), roms.end());

        std::string detail = console.slug;
        std::transform(detail.begin(), detail.end(), detail.begin(),
                       [](unsigned char c) { return (char)std::toupper(c); });
        for (const std::string &path : roms)
        {
            if (!g_librarySlugs.emplace(path, console.slug).second)
                continue; // listed under the first console that has it
            const std::string filename = path.substr(path.find_last_of('/') + 1);
            std::string title = TicoUtils::GetCleanTitle(filename);
            if (title.empty())
                title = filename;
            entries.push_back({title, detail, path});
        }
    }
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
        return strcasecmp(a.title.c_str(), b.title.c_str()) < 0;
    });
    return entries;
}

static std::string LibrarySlugFor(const std::string &path)
{
    const auto it = g_librarySlugs.find(path);
    return it != g_librarySlugs.end() ? it->second : SlugForRom(path);
}

static void RegisterLibrary()
{
    OverlayUI::LibraryCallbacks library;
    library.list = [] { return ListLibrary(); };
    library.launch = [](const std::string &path) { g_pendingLaunch = path; };
    OverlayUI::SetLibraryCallbacks(std::move(library));

    OverlayUI::LibraryFolderCallbacks folders;
    folders.groups = [] {
        std::vector<OverlayUI::LibraryFolderGroup> groups;
        const std::vector<std::string> bases = TicoRomBases();
        for (const LibraryConsole &console : kLibraryConsoles)
        {
            OverlayUI::LibraryFolderGroup group;
            group.label = console.title;
            for (const std::string &base : bases)
                group.bases.push_back(base + console.slug + "/");
            group.folders = ModuleRomFolders(console.slug);
            groups.push_back(std::move(group));
        }
        return groups;
    };
    folders.set = [](int group, const std::vector<std::string> &paths) {
        if (group >= 0 && group < (int)(sizeof(kLibraryConsoles) / sizeof(kLibraryConsoles[0])))
        {
            std::vector<std::string> normalized;
            for (const std::string &path : paths)
                normalized.push_back(WithSlash(path));
            SetModuleRomFolders(kLibraryConsoles[group].slug, normalized);
        }
    };
    OverlayUI::SetLibraryFolderCallbacks(std::move(folders));
}

// Creates the core for a game and loads it. The console picks the save and
// state folders, so it is set before the core, which reads them when created.
static void StartGame(const std::string &slug, const std::string &romArg, const std::string &titleArg)
{
    // tico names a game on a USB drive by the drive's id: find where it is mounted
    std::string romPath = UsbStorage::Resolve(romArg);
    if (romPath.empty())
    {
        LOG_ERROR("HOME", "USB drive for %s is not connected", romArg.c_str());
        romPath = romArg;
    }
    TicoConfig::SetSlug(slug);
    g_titleArg = titleArg;
    LOG_INFO("HOME", "Console slug: %s, ROM: %s", slug.c_str(), romPath.c_str());
    TicoConfig::MakeDirs(TicoConfig::SavesPath());
    TicoConfig::MakeDirs(TicoConfig::StatesPath());
    TicoConfig::MakeDirs(TicoConfig::SystemPath());

    // this game's own settings (Settings > This Game), if it has them, over the core's
    if (!woplay_host::active()) OverlayConfig::SetGame(romPath);
    g_core = std::make_unique<TicoCore>();
    g_core->EnsureConfigLoaded();
    ApplySettingsToCore();
    g_core->SetAudioCallbacks(AudioSampleCallback, AudioSampleBatchCallback, AudioFlushCallback);
    g_core->SetVideoCallback(VideoCallback);

    const size_t lastSlash = romPath.find_last_of("/\\");
    const std::string filename = lastSlash != std::string::npos ? romPath.substr(lastSlash + 1) : romPath;
    // Prefer the launcher-supplied title; fall back to the rom filename.
    std::string cleanTitle = titleArg.empty() ? TicoUtils::GetCleanTitle(filename) : titleArg;
    if (cleanTitle.empty())
        cleanTitle = filename;
    OverlayUI::SetGameTitle(cleanTitle);
    OverlayUI::SetLibraryMode(false);

    if (!g_core->LoadGame(romPath))
    {
        LOG_ERROR("HOME", "Failed to load ROM: %s", romPath.c_str());
        if (g_standalone)
        {
            ShowLibrary();
            OverlayUI::ShowToast(SwitchFrontend::OverlayTranslation::tr("emulator_load_game_failed"),
                                 OverlayUI::ToastCorner::TopRight);
        }
        return;
    }
    if (woplay_host::active()) woplay_host::beginGame();
    g_pacedFps = 0.0; // UpdateFramePacing sets the audio rate for this game
    g_offerResume = true;
    if (g_core->ConsumeCheatsRecovered())
        OverlayUI::ShowToast(SwitchFrontend::OverlayTranslation::tr("emulator_cheats_recovered"),
                             OverlayUI::ToastCorner::TopRight);
}

// Back to the library: the core unloads (saving the game and its clock).
static void ShowLibrary()
{
    if (g_core)
    {
        AutoSaveState();
        TicoVulkan::WaitIdle();
        g_core.reset();
        TicoCore::StopRumble();
    }
    StopFastForward();
    OverlayUI::SetGameTitle("mGBA");
    OverlayConfig::SetGame(std::string()); // the library has no game settings
    OverlayUI::SetLibraryMode(true);
    if (g_menuOpen)
        CloseMenu();
    OpenMenu();
}

void HandleInput()
{
    SDL_GameController *controllers[4] = {nullptr, nullptr, nullptr, nullptr};
    int numControllers = 0;

    if (g_controllersDirty)
    {
        RefreshControllers();
    }

    for (int i = 0; i < 4; ++i)
    {
        if (g_controllers[i])
            controllers[numControllers++] = g_controllers[i];
    }

    RunMenuAction();
    if (!g_running)
        return;

    if (!g_pendingLaunch.empty())
    {
        const std::string path = g_pendingLaunch;
        g_pendingLaunch.clear();
        CloseMenu();
        StartGame(LibrarySlugFor(path), path, std::string());
        return;
    }

    SDL_GameController *pad = numControllers > 0 ? controllers[0] : nullptr;
    if (woplay_host::active()) {
        const auto triggerMask = SwitchBit(SwitchButton::ZL) | SwitchBit(SwitchButton::ZR);
        const auto held = pad ? SwitchButtonsHeld(pad) : 0;
        const bool toggle = (held & triggerMask) == triggerMask;
        if (toggle && !g_toggleHeld && g_core && !woplay_menu::bindingActive()) {
            if (g_menuOpen) CloseMenu();
            else OpenMenu();
        }
        g_toggleHeld = toggle;
        const auto xMask = SwitchBit(SwitchButton::X);
        const auto leftStick = SwitchBit(SwitchButton::StickL);
        const auto rightStick = SwitchBit(SwitchButton::StickR);
        const auto shortcutMask = xMask | leftStick | rightStick;
        const auto shortcut = held & shortcutMask;
        const bool quickSave = !toggle && shortcut == (xMask | leftStick);
        const bool quickLoad = !toggle && shortcut == (xMask | rightStick);
        static bool quickHeld = false;
        const bool quickPress = (quickSave || quickLoad) && !quickHeld;
        quickHeld = quickSave || quickLoad;
        if (g_menuOpen) {
            FeedMenuTouch();
            return;
        }
        if (g_waitGameInput) {
            if (g_core) g_core->ClearInputs();
            g_waitGameInput = (held & (triggerMask | shortcutMask | SwitchBit(SwitchButton::A) | SwitchBit(SwitchButton::B))) != 0;
            return;
        }
        if (quickPress && g_core) {
            using woplay_host::StateGroup;
            using woplay_host::StateOperation;
            g_core->ClearInputs();
            RunWoPlayState({quickSave ? StateOperation::Save : StateOperation::Load,StateGroup::Cycle});
            g_waitGameInput = true;
            return;
        }
    } else if (pad && g_overlayReady)
    {
        // Guide, or Plus+Minus, opens the menu and closes it again.
        const bool start = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START);
        const bool select = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK);
        const bool guide = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_GUIDE);
        const bool toggle = guide || (start && select);
        // the library stays open while no game runs
        if (toggle && !g_toggleHeld && g_core)
        {
            if (g_menuOpen)
                CloseMenu();
            else
                OpenMenu();
        }
        g_toggleHeld = toggle;
        if (toggle && g_core)
        {
            g_core->ClearInputs();
            return;
        }
    }
    if (g_menuOpen)
    {
        if (pad)
            FeedMenu(pad);
        FeedMenuTouch();
        return;
    }

    if (g_core)
    {
        g_core->ClearInputs();

        // Player 1's fast-forward hotkey: its Switch button is not mapped.
        const uint32_t ffButton = UpdateFastForward(numControllers > 0 ? controllers[0] : nullptr);
        const bool analogDpad = OverlayConfig::GetConfigValue("analog_dpad", "enabled") != "disabled";

        for (int p=0; p<std::max(woplay_host::active()?1:0,numControllers); ++p)
        {
            SDL_GameController *controller=p<numControllers?controllers[p]:nullptr;
            if(!controller&&p>0)continue;
            uint32_t held=controller?SwitchButtonsHeld(controller):0;
            if (p == 0)
                held &= ~ffButton;
            if (analogDpad && controller)
            {
                const int16_t leftX = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX);
                const int16_t leftY = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY);
                if (leftY < -16000) held |= SwitchBit(SwitchButton::Up);
                if (leftY > 16000) held |= SwitchBit(SwitchButton::Down);
                if (leftX < -16000) held |= SwitchBit(SwitchButton::Left);
                if (leftX > 16000) held |= SwitchBit(SwitchButton::Right);
            }

            // Controls > Button mapping: each GBA button on its Switch button.
            for (const ButtonMapping &mapping : kButtonMappings)
            {
                g_core->SetInputState(p,mapping.retroId,
                    MappingHeld(OverlayConfig::GetConfigValue(mapping.key,mapping.fallback),held,p==0));
            }
        }
    }
}

void Render()
{
    static int frameCount = 0;
    frameCount++;

    if (frameCount <= 3)
    {
        LOG_DEBUG("RENDER", "Frame %d: Render starting", frameCount);
    }

#ifdef __SWITCH__
    UpdateScreenMode();
#endif

    // Waits for this frame slot's previous submission, so everything below
    // may reuse per-frame resources. A skipped frame (swapchain being
    // recreated) still runs the core so emulation keeps its pace.
    VkCommandBuffer cmd = TicoVulkan::BeginFrame();

#ifdef __SWITCH__
    ImGuiIO &io = ImGui::GetIO();
    int logW, logH;
    GetDisplayResolution(logW, logH);
    io.DisplaySize = ImVec2((float)logW, (float)logH);
    io.DeltaTime = 1.0f / 60.0f;
#else
    ImGui_ImplSDL2_NewFrame();
    int logW, logH;
    GetDisplayResolution(logW, logH);
    ImGui::GetIO().DisplaySize = ImVec2((float)logW, (float)logH);
    ImGui::GetIO().DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
#endif
    if (woplay_host::active() && g_menuOpen) {
        const auto pad = std::find_if(std::begin(g_controllers),std::end(g_controllers),
                                      [](const auto* controller) { return controller != nullptr; });
        woplay_menu::input(pad == std::end(g_controllers) ? nullptr : *pad);
    }
    ImGui::NewFrame();

    int w, h;
    GetDisplayResolution(w, h);
    ImVec2 displaySize((float)w, (float)h);

    if (g_core && !g_menuOpen)
    {
        if (frameCount <= 3)
            LOG_DEBUG("RENDER", "Frame %d: Calling RunFrame", frameCount);
        UpdateFramePacing();
        g_frameAccum += g_frameStep * FramesThisRefresh();
        while (g_frameAccum >= 1.0)
        {
            g_frameAccum -= 1.0;
            g_core->RunFrame();
        }
        if (g_offerResume)
            OfferResume();
    }
    else if (g_core)
    {
        // paused in the menu: keep the RetroAchievements session alive
        g_core->Idle();
    }

    ApplyShaderPreset();
    const auto gameFrame = DrawGame(cmd, ImGui::GetBackgroundDrawList(), displaySize);
    UpdateHud(ImGui::GetIO().DeltaTime);
    if (woplay_host::active())
        woplay_menu::draw(g_core.get(),gameFrame);
    else {
        ImGuiOverlay::Draw(g_core.get(), displaySize.x, displaySize.y, ImGui::GetIO().DeltaTime);
    }

    if (g_core && g_core->GetOSDFrames() > 0)
    {
        ImDrawList *fg = ImGui::GetForegroundDrawList();
        const float marginX = 24.0f;
        const float marginY = 16.0f;
        const float padX = 16.0f;
        const float padY = 8.0f;
        const float rounding = 14.0f;
        
        int frames = g_core->GetOSDFrames();
        float alpha = 1.0f;
        if (frames < 30) alpha = frames / 30.0f;
        
        std::string msg = g_core->GetOSDMessage();
        ImVec2 textSize = ImGui::CalcTextSize(msg.c_str());
        
        float pillW = textSize.x + padX * 2;
        float pillH = textSize.y + padY * 2;
        float pillX = marginX;
        float pillY = marginY;
        
        ImU32 bgCol = IM_COL32(0, 0, 0, (int)(alpha * 153));
        fg->AddRectFilled(ImVec2(pillX, pillY), ImVec2(pillX + pillW, pillY + pillH), bgCol, rounding);
        
        ImU32 textCol = IM_COL32(255, 255, 255, (int)(alpha * 240));
        fg->AddText(ImVec2(pillX + padX, pillY + padY), textCol, msg.c_str());
        
        g_core->DecrementOSD();
    }

    ImGui::Render();
    if (cmd) {
        auto* frame = ImGui::GetDrawData();
        if (woplay_host::active()) woplay_menu::prepareEffects(cmd,*frame);
        TicoVulkan::EndFrame(frame);
    }
}

//==============================================================================
// Main
//==============================================================================

int main(int argc, char *argv[])
{
    std::setlocale(LC_ALL, "");
#ifdef WOPLAY_HOST
    try {
        woplay_host::initialize(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "WoPlay startup: %s\n", error.what());
        woplay_host::finish(error.what());
        return 2;
    }
#endif
    tico::UseShaderCacheDir(woplay_host::active() ? woplay_host::directory("cache") + "/mesa"
                                                  : "sdmc:/tico/system/mgba/cache/mesa");
    Logger::Instance().ResetLogFile();

    g_running = true;
    g_controllersDirty = true;

#ifdef __SWITCH__
    LOG_INFO("HOME", "Calling appletLockExit...");
    appletLockExit();
    LOG_INFO("HOME", "Calling romfsInit...");
    Result romfsRc = romfsInit();
    if (R_FAILED(romfsRc))
    {
        LOG_WARN("HOME", "romfsInit failed: 0x%x", romfsRc);
    }
    else
    {
        LOG_INFO("HOME", "romfsInit succeeded");
    }

    LOG_INFO("HOME", "Calling nwindowSetDimensions...");
    nwindowSetDimensions(nwindowGetDefault(), 1920, 1080);
    LOG_INFO("HOME", "Switch pre-init complete (romfs, nwindow)");

    if (R_SUCCEEDED(socketInitializeDefault()))
    {
        LOG_INFO("HOME", "socketInitializeDefault succeeded");
    }
    else
    {
        LOG_ERROR("HOME", "socketInitializeDefault failed");
    }

    // USB drives mount in the background while the rest starts
    UsbStorage::Init();
#endif

    LOG_INFO("HOME", "mgba starting (slug: %s)...", TicoConfig::CURRENT_SLUG.c_str());

    LOG_INFO("HOME", "Calling InitWindow...");
    if (!InitWindow())
    {
        LOG_ERROR("HOME", "Failed to initialize window");
        Logger::Instance().CloseLogFile();
        woplay_host::finish("Window initialization failed");
        return 1;
    }
    LOG_INFO("HOME", "InitWindow succeeded");

#ifdef __SWITCH__
    PinCurrentThreadToCore(2, "main/render");
#endif

    LOG_INFO("HOME", "Calling InitImGui...");
    if (!InitImGui())
    {
        LOG_ERROR("HOME", "Failed to initialize ImGui");
        CleanupWindow();
        Logger::Instance().CloseLogFile();
        woplay_host::finish("Vulkan or UI initialization failed");
        return 1;
    }
    LOG_INFO("HOME", "InitImGui succeeded");

    g_chain = std::make_unique<TicoShaderChain>();
    if (!g_chain->Init())
    {
        LOG_ERROR("HOME", "Shader chain initialization failed");
        g_chain.reset();
    }
#ifdef __SWITCH__
    g_lastOperationMode = 255;
#endif

    if (!woplay_host::active()) {
        OverlayConfig::ReloadConfig();
        MigrateShaderSetting();
    }

    if (!g_audio.Init(g_audioDevice))
    {
        LOG_WARN("HOME", "TicoAudio init failed");
    }

    g_overlayReady = woplay_host::active() ? woplay_menu::initialize(ApplySettingsToCore) : ImGuiOverlay::Init();
    std::string woplayError;
    if (woplay_host::active() && !g_overlayReady) {
        woplayError = "WoPlay menu initialization failed";
        g_running = false;
    }
    if (!woplay_host::active()) {
    // Save/Load State show each slot's picture and when it was saved.
    static std::array<ImTextureID, 6> slotPictures{};
    OverlayUI::SetSlotPreviewCallback([](int slot) {
        OverlayUI::SlotPreview preview;
        if (slot < 1 || slot > (int)slotPictures.size() || !g_core)
            return preview;
        ImTextureID &picture = slotPictures[slot - 1];
        TicoVulkan::DestroyTexture(picture); // the slot may have been saved again
        picture = ImTextureID_Invalid;
        const std::string path = StatePath(slot - 1);
        struct stat st;
        if (stat(path.c_str(), &st) != 0)
            return preview;
        char when[32];
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", std::localtime(&st.st_mtime));
        preview.saved_at = when;
        int w = 0, h = 0, channels = 0;
        if (unsigned char *rgba = stbi_load((path + ".png").c_str(), &w, &h, &channels, 4))
        {
            picture = TicoVulkan::CreateTextureRGBA(rgba, w, h);
            stbi_image_free(rgba);
        }
        preview.texture = (unsigned long long)picture;
        if (g_core->GetAspectRatio() > 0.1f)
            preview.aspect = g_core->GetAspectRatio();
        return preview;
    });
    // Cheats from the game's .cheats file; the menu hides them in hardcore.
    OverlayUI::SetCheatCallbacks(
        [] {
            std::vector<OverlayUI::CheatMenuEntry> entries;
            if (!g_core)
                return entries;
            const auto &cheats = g_core->GetCheats();
            for (size_t i = 0; i < cheats.size(); ++i)
                entries.push_back({cheats[i].name, cheats[i].enabled, true, (int)i, false});
            return entries;
        },
        [](int index) {
            if (!g_core || index < 0)
                return false;
            g_core->ToggleCheat((size_t)index);
            return true;
        });
    OverlayUI::SetSlotOccupiedCallback([](int slot) {
        struct stat st;
        return g_core && slot >= 1 && stat(StatePath(slot - 1).c_str(), &st) == 0;
    });
        OverlayUI::ReloadSettings();
    }
    RegisterShaderMenu();

    // tico launches with argv[1] = console slug, argv[2] = ROM path,
    // argv[3] = title. Without a ROM (e.g. from the homebrew menu) the library
    // lists the ROM folders instead.
    if (woplay_host::active()) {
        if (g_running && woplay_host::launchingGame()) {
            StartGame(woplay_host::platform(), woplay_host::rom(), woplay_host::title());
            if (!g_core || !g_core->IsGameLoaded()) {
                g_running = false;
                woplayError = "ROM load failed";
            }
        } else if (g_running) g_menuOpen = true;
    } else if (argc >= 3 && strchr(argv[1], '/'))
    {
        // tico before {slug} in the launch line: argv[1] = ROM, argv[2] = title
        StartGame(SlugForRom(argv[1]), argv[1], argv[2] ? argv[2] : "");
    }
    else if (argc >= 3)
    {
        StartGame(argv[1], argv[2], argc >= 4 && argv[3] ? argv[3] : "");
    }
    else if (argc == 2)
    {
        // a single argument is the ROM path
        StartGame(SlugForRom(argv[1]), argv[1], std::string());
    }
    else
    {
        g_standalone = true;
        RegisterLibrary();
        ShowLibrary();
    }

    // Frame pacing is handled entirely by vsync (FIFO presentation). Audio is
    // non-blocking, so presentation is the only governor. While fast-forwarding
    // the swapchain switches to an uncapped present mode.
    bool lastFastForward = false;

    while (g_running)
    {
#ifdef __SWITCH__
        if (!appletMainLoop())
        {
            LOG_INFO("HOME", "appletMainLoop returned false, exiting main loop");
            g_running = false;
            break;
        }
#endif

        bool fastForward = FastForwardUncapped();
        if (fastForward != lastFastForward)
        {
            TicoVulkan::SetVsync(!fastForward);
            lastFastForward = fastForward;
        }

        ProcessEvents();
        HandleInput();
        Render();
    }

    LOG_INFO("HOME", "Starting cleanup...");
    if (!g_woplayAutoSaved) AutoSaveState();
    TicoVulkan::WaitIdle();
    if (woplay_host::active()) woplay_menu::shutdown();
    OverlayUI::SetSlotOccupiedCallback(nullptr);
    OverlayUI::SetSlotPreviewCallback(nullptr);
    OverlayUI::SetCheatCallbacks(nullptr, nullptr);
    OverlayUI::SetShaderCallbacks({});
    OverlayUI::SetLibraryCallbacks({});
    OverlayUI::SetLibraryFolderCallbacks({});
    if (!woplay_host::active()) ImGuiOverlay::Shutdown();
    g_core.reset();
    TicoCore::StopRumble();



    g_audio.Shutdown();
    if (!TicoConfig::USE_SDLQUEUEAUDIO)
    {
        Mix_CloseAudio();
    }

    CleanupWindow();
    if (woplay_host::active()) woplay_host::finish(woplayError);

#ifdef __SWITCH__
    UsbStorage::Shutdown(); // flush and unmount before tico takes over again
    socketExit();
    romfsExit();
    appletUnlockExit();
#endif

    LOG_INFO("HOME", "Clean exit");
    Logger::Instance().CloseLogFile();

    return woplayError.empty() ? 0 : 1;
}
