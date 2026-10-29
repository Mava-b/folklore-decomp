/*
 * engine_settings.cpp -- load, save and apply the port's settings.
 *
 * The file is plain `key = value` lines. Its path is $FOLKLORE_SETTINGS
 * (run.ps1 sets I:\Recomp\Folklore\folklore_settings.ini), else
 * folklore_settings.ini in the working directory.
 */
#include "engine_settings.h"
#include "engine_region.h"

#include <cmath>
#include <cstdio>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace
{
    EngineSettings settingsInstance;
    bool settingsLoaded = false;

    /* Folklore's main loops measure the frame in 10 us units since the mark
     * at the top of the frame and sleep until it passes 3000 (30 ms); one
     * mode uses 4700 (47 ms). */
    constexpr int kOriginalThirtyTarget = 3000;

    void trimInPlace(char* text)
    {
        char* start = text;
        while (*start == ' ' || *start == '\t')
        {
            start++;
        }
        if (start != text)
        {
            std::memmove(text, start, std::strlen(start) + 1);
        }
        size_t length = std::strlen(text);
        while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                              text[length - 1] == '\r' || text[length - 1] == '\n'))
        {
            text[--length] = '\0';
        }
    }

    /* Load before any guest code runs, so the limiter (game thread) and the
     * menu (present thread) never race over the first lazy load. */
    struct LoadAtStartup
    {
        LoadAtStartup()
        {
            engineSettingsLoad();
            engineSettingsApplyGraphics();
            engineSettingsApplyInput();
        }
    };

    bool parseBool(const char* value)
    {
        return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
               std::strcmp(value, "on") == 0 || std::strcmp(value, "yes") == 0;
    }

    struct LanguageEntry
    {
        GameLanguage language;
        const char* key;    /* in folklore_settings.ini */
        const char* name;   /* in the menu and the log */
    };

    const LanguageEntry kLanguages[] = {
        { GameLanguage::English, "english", "English" },
        { GameLanguage::French,  "french",  "French" },
        { GameLanguage::Spanish, "spanish", "Spanish" },
        { GameLanguage::German,  "german",  "German" },
        { GameLanguage::Italian, "italian", "Italian" },
    };

    const LanguageEntry& languageEntry(GameLanguage language)
    {
        for (const LanguageEntry& entry : kLanguages)
        {
            if (entry.language == language)
            {
                return entry;
            }
        }
        return kLanguages[0];
    }

    /* 0 until the game first asks, then the CELL_SYSUTIL_LANG_* value it got. */
    int activeLanguageCode = 0;
}

LoadAtStartup loadAtStartup;

EngineSettings& engineSettings()
{
    if (!settingsLoaded)
    {
        engineSettingsLoad();
    }
    return settingsInstance;
}

std::string engineExecutableDirectory()
{
    char modulePath[MAX_PATH * 2] = {};
    const DWORD length = GetModuleFileNameA(nullptr, modulePath, (DWORD)sizeof(modulePath));
    if (length == 0 || length >= sizeof(modulePath))
    {
        return ".";
    }
    std::string directory(modulePath, length);
    const size_t separator = directory.find_last_of("\\/");
    return separator == std::string::npos ? std::string(".") : directory.substr(0, separator);
}

const char* engineSettingsPath()
{
    const char* path = std::getenv("FOLKLORE_SETTINGS");
    if (path && *path)
    {
        return path;
    }
    /* Next to the exe, not the working directory: a shared build is started
     * by double-clicking, from wherever. */
    static const std::string besideExecutable = engineExecutableDirectory() + "\\folklore_settings.ini";
    return besideExecutable.c_str();
}

void engineSettingsLoad()
{
    settingsLoaded = true;
    settingsInstance = EngineSettings();
    FILE* file = std::fopen(engineSettingsPath(), "r");
    if (!file)
    {
        return;
    }
    char line[2048];
    while (std::fgets(line, sizeof(line), file))
    {
        char* separator = std::strchr(line, '=');
        if (!separator || line[0] == '#' || line[0] == ';')
        {
            continue;
        }
        *separator = '\0';
        char* key = line;
        char* value = separator + 1;
        trimInPlace(key);
        trimInPlace(value);
        if (std::strcmp(key, "frame_rate_cap") == 0)
        {
            if (std::strcmp(value, "60") == 0)
            {
                settingsInstance.frameRateCap = FrameRateCap::Sixty;
            }
            else if (std::strcmp(value, "unlimited") == 0)
            {
                settingsInstance.frameRateCap = FrameRateCap::Unlimited;
            }
            else
            {
                settingsInstance.frameRateCap = FrameRateCap::Original;
            }
        }
        else if (std::strcmp(key, "fine_limiter_sleep") == 0)
        {
            settingsInstance.fineLimiterSleep = parseBool(value);
        }
        else if (std::strcmp(key, "show_performance_overlay") == 0)
        {
            settingsInstance.showPerformanceOverlay = parseBool(value);
        }
        else if (std::strcmp(key, "render_scale") == 0)
        {
            const int scale = std::atoi(value);
            settingsInstance.renderScale = (scale >= 1 && scale <= 3) ? scale : 1;
        }
        else if (std::strcmp(key, "anti_aliasing") == 0)
        {
            settingsInstance.antiAliasing = std::strcmp(value, "fxaa") == 0 ? AntiAliasing::Fxaa
                                                                          : AntiAliasing::Off;
        }
        else if (std::strcmp(key, "background_shaders") == 0)
        {
            settingsInstance.backgroundShaders = parseBool(value);
        }
        else if (std::strcmp(key, "anisotropic_filtering") == 0)
        {
            const int level = std::atoi(value);
            settingsInstance.anisotropy = (level == 2 || level == 4 || level == 8 || level == 16) ? level : 1;
        }
        else if (std::strcmp(key, "sharpening") == 0)
        {
            const int percent = std::atoi(value);
            settingsInstance.sharpening = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
        }
        else if (std::strcmp(key, "aspect_ratio") == 0)
        {
            if (std::strcmp(value, "display") == 0)
            {
                settingsInstance.aspectRatio = AspectRatio::MatchDisplay;
            }
            else if (std::strcmp(value, "21:9") == 0)
            {
                settingsInstance.aspectRatio = AspectRatio::Ultrawide;
            }
            else if (std::strcmp(value, "32:9") == 0)
            {
                settingsInstance.aspectRatio = AspectRatio::SuperUltrawide;
            }
            else
            {
                settingsInstance.aspectRatio = AspectRatio::Original;
            }
        }
        else if (std::strcmp(key, "motion_controls") == 0)
        {
            /* on / off; the older automatic / sixaxis / auto_capture choice
             * reads as on, on, off. */
            settingsInstance.motionControls = !(std::strcmp(value, "off") == 0 || std::strcmp(value, "0") == 0 ||
                                                std::strcmp(value, "false") == 0 ||
                                                std::strcmp(value, "auto_capture") == 0);
        }
        else if (std::strcmp(key, "physics_frame_step") == 0)
        {
            settingsInstance.physicsFrameStep = parseBool(value);
        }
        else if (std::strcmp(key, "fullscreen") == 0)
        {
            settingsInstance.fullscreen = parseBool(value);
        }
        else if (std::strcmp(key, "language") == 0)
        {
            settingsInstance.language = GameLanguage::English;
            for (const LanguageEntry& entry : kLanguages)
            {
                if (_stricmp(value, entry.key) == 0)
                {
                    settingsInstance.language = entry.language;
                }
            }
        }
        else if (std::strcmp(key, "game_dir") == 0)
        {
            settingsInstance.gameDir = value;
        }
        else if (std::strcmp(key, "update_dir") == 0)
        {
            settingsInstance.updateDir = value;
        }
        else if (std::strcmp(key, "dev_flash") == 0)
        {
            settingsInstance.devFlashDir = value;
        }
        else if (std::strcmp(key, "data_dir") == 0)
        {
            settingsInstance.dataDir = value;
        }
    }
    std::fclose(file);
    std::fprintf(stderr, "[engine] settings loaded from %s\n", engineSettingsPath());
}

void engineSettingsSave()
{
    FILE* file = std::fopen(engineSettingsPath(), "w");
    if (!file)
    {
        std::fprintf(stderr, "[engine] could not write %s\n", engineSettingsPath());
        return;
    }
    const EngineSettings& current = settingsInstance;
    const char* capText = current.frameRateCap == FrameRateCap::Sixty ? "60"
                        : current.frameRateCap == FrameRateCap::Unlimited ? "unlimited"
                        : "original";
    std::fprintf(file, "# Folklore port settings (the L3+R3 / F1 engine menu writes this file)\n");
    std::fprintf(file, "frame_rate_cap = %s\n", capText);
    std::fprintf(file, "fine_limiter_sleep = %d\n", current.fineLimiterSleep ? 1 : 0);
    std::fprintf(file, "show_performance_overlay = %d\n", current.showPerformanceOverlay ? 1 : 0);
    std::fprintf(file, "render_scale = %d\n", current.renderScale);
    std::fprintf(file, "anti_aliasing = %s\n", current.antiAliasing == AntiAliasing::Fxaa ? "fxaa" : "off");
    std::fprintf(file, "background_shaders = %d\n", current.backgroundShaders ? 1 : 0);
    std::fprintf(file, "anisotropic_filtering = %d\n", current.anisotropy);
    std::fprintf(file, "sharpening = %d\n", current.sharpening);
    std::fprintf(file, "physics_frame_step = %d\n", current.physicsFrameStep ? 1 : 0);
    std::fprintf(file, "# on: capture Folks by moving the controller (DualShock 3/4, DualSense, Switch Pro); off: captures complete by themselves\n");
    std::fprintf(file, "motion_controls = %s\n", current.motionControls ? "on" : "off");
    const char* aspectText = current.aspectRatio == AspectRatio::MatchDisplay ? "display"
                           : current.aspectRatio == AspectRatio::Ultrawide ? "21:9"
                           : current.aspectRatio == AspectRatio::SuperUltrawide ? "32:9"
                           : "16:9";
    std::fprintf(file, "aspect_ratio = %s\n", aspectText);
    std::fprintf(file, "fullscreen = %d\n", current.fullscreen ? 1 : 0);
    std::fprintf(file, "# European release only: english, french, spanish, german or italian (next launch)\n");
    std::fprintf(file, "language = %s\n", languageEntry(current.language).key);
    std::fprintf(file, "# The folder that holds PS3_GAME (with the decrypted PS3_GAME\\USRDIR\\EBOOT.elf)\n");
    std::fprintf(file, "game_dir = %s\n", current.gameDir.c_str());
    std::fprintf(file, "# 1.10 builds only: the installed 1.10 update (RPCS3: dev_hdd0\\game\\<title id>), with the decrypted USRDIR\\EBOOT.elf\n");
    std::fprintf(file, "update_dir = %s\n", current.updateDir.c_str());
    std::fprintf(file, "# Optional: a firmware dev_flash folder (RPCS3's), for the system fonts\n");
    std::fprintf(file, "dev_flash = %s\n", current.devFlashDir.c_str());
    std::fprintf(file, "# Where hdd0/hdd1 (installed data), gamedata (saves) and logs go; empty = next to folklore.exe\n");
    std::fprintf(file, "data_dir = %s\n", current.dataDir.c_str());
    std::fclose(file);
}

extern "C"
{
    void rsx_live_draw_set_render_scale(uint32_t scale);
    void rsx_live_draw_set_post_aa(int mode);
    void rsx_live_draw_set_async_shaders(int enabled);
    void rsx_live_draw_set_anisotropy(uint32_t level);
    void rsx_live_draw_set_sharpening(uint32_t percent);
    int cellPad_set_motion_mode(int mode);
    void rsx_live_draw_set_aspect(float aspect);
    float rsx_live_draw_get_aspect(void);
    void folklore_apply_aspect(float aspect);
    void rsx_null_backend_set_fullscreen(int enabled);
}

void engineSettingsApplyGraphics()
{
    const EngineSettings& current = engineSettings();
    rsx_live_draw_set_render_scale((uint32_t)current.renderScale);
    rsx_live_draw_set_post_aa(current.antiAliasing == AntiAliasing::Fxaa ? 1 : 0);
    rsx_live_draw_set_async_shaders(current.backgroundShaders ? 1 : 0);
    rsx_live_draw_set_anisotropy((uint32_t)current.anisotropy);
    rsx_live_draw_set_sharpening((uint32_t)current.sharpening);
    rsx_live_draw_set_aspect(engineSettingsAspectValue(current.aspectRatio));
    rsx_null_backend_set_fullscreen(current.fullscreen ? 1 : 0);
}

float engineSettingsAspectValue(AspectRatio aspectRatio)
{
    switch (aspectRatio)
    {
    case AspectRatio::MatchDisplay:
    {
        const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        if (screenWidth > 0 && screenHeight > 0)
        {
            const float displayAspect = (float)screenWidth / (float)screenHeight;
            return displayAspect > 16.0f / 9.0f ? displayAspect : 16.0f / 9.0f;
        }
        return 16.0f / 9.0f;
    }
    case AspectRatio::Ultrawide:
        return 64.0f / 27.0f;
    case AspectRatio::SuperUltrawide:
        return 32.0f / 9.0f;
    case AspectRatio::Original:
    default:
        return 16.0f / 9.0f;
    }
}

void engineSettingsApplyInput()
{
    /* cellPad.c's modes: 0 = the controller's sensors when it has them,
     * otherwise automatic capture; 2 = automatic capture always. */
    cellPad_set_motion_mode(engineSettings().motionControls ? 0 : 2);
}

bool engineSettingsLanguageSelectable()
{
#if defined(FOLKLORE_REGION_EU)
    return true;
#else
    return false;
#endif
}

const char* engineSettingsLanguageName(GameLanguage language)
{
    return languageEntry(language).name;
}

namespace
{
    std::mutex languageLock;

    GameLanguage languageToGive()
    {
        if (engineSettingsLanguageSelectable())
        {
            return engineSettings().language;
        }
        return GameLanguage::English;
    }
}

GameLanguage engineSettingsActiveLanguage()
{
    std::lock_guard<std::mutex> guard(languageLock);
    if (activeLanguageCode == 0)
    {
        /* Not asked yet: the setting is what it will get. */
        return languageToGive();
    }
    return (GameLanguage)activeLanguageCode;
}

/* The game asks as it starts (func_0020C324 in the EU executable,
 * func_0020C320 in the US one) and keeps the answer in a global its text,
 * subtitle and movie loaders index by, so the first answer is the run's
 * language. The EU executable turns anything but CELL_SYSUTIL_LANG 1-5 into
 * English (its "_uk" files). The US one does the same into "_en", but its
 * disc has only the English subtitle files, so it is always told English. */
extern "C" int folklore_system_language(void)
{
    std::lock_guard<std::mutex> guard(languageLock);
    if (activeLanguageCode == 0)
    {
        const GameLanguage language = languageToGive();
        activeLanguageCode = (int)language;
        std::fprintf(stderr, "[engine] game language: %s (%s release)\n", engineSettingsLanguageName(language),
                     FOLKLORE_RELEASE_NAME);
    }
    return activeLanguageCode;
}

extern "C" int folklore_limiter_target(int originalTarget)
{
    /* The first frame end: the renderer is up, so widen the game's
     * projection to exactly the aspect it presents at. */
    static bool aspectApplied = false;
    if (!aspectApplied)
    {
        aspectApplied = true;
        const float activeAspect = rsx_live_draw_get_aspect();
        if (activeAspect > 16.0f / 9.0f + 0.01f)
        {
            folklore_apply_aspect(activeAspect);
        }
    }
    switch (engineSettings().frameRateCap)
    {
    case FrameRateCap::Sixty:
        /* 30 fps -> 60 fps; the 47 ms mode scaled by the same factor. The
         * target sits a little under 16.7 ms so the flip, not the limiter,
         * paces the frame: at exactly 16.66 ms the limiter's own wake-up
         * slack (up to a sleep interval) pushed most frames past a vblank
         * and the game settled around 56 fps. */
        return originalTarget == kOriginalThirtyTarget ? 1600 : originalTarget / 2;
    case FrameRateCap::Unlimited:
        return 0;
    case FrameRateCap::Original:
    default:
        return originalTarget;
    }
}

extern "C" int folklore_limiter_sleep_us(int originalSleepMicroseconds)
{
    if (engineSettings().fineLimiterSleep && originalSleepMicroseconds > 1000)
    {
        return 1000;
    }
    return originalSleepMicroseconds;
}

/*
 * Folklore steps every Havok world through one helper (func_003C913C, from
 * func_0004F188): setFrameTimeMarker(frameTime), then stepDeltaTime(1/30)
 * until the simulation reaches the marker. The character cloth and hair
 * objects (32 slots of 0x75B0 bytes, func_0002BA64) drive their anchor
 * bodies with hard keyframes whose velocity is (target - position) / frameTime.
 * At 30 fps that is one 1/30 step per frame and the anchors land on their
 * targets. Above 30 fps the steps no longer line up with the frames: some
 * frames take no step, others take one 1/30 step with a velocity worked out
 * for a 1/60 frame, or two steps after a long frame -- the anchors overshoot
 * by 2-4x and snap back, and the cloth hanging off them whips around.
 *
 * With this on, each frame takes exactly the time to the marker in steps of
 * at most ~1.15x the game's own step: one step of the frame time at 30-60+
 * fps, the same 1/30 step as the original at 30 fps, and equal substeps when
 * a frame runs long. A frame shorter than 2 ms takes no step; its time is
 * carried into the next frame's marker.
 *
 * Returns the number of steps to take (0 = none) and the step size, or -1 to
 * run the game's original loop.
 */
extern "C" int folklore_physics_plan_steps(float currentTime, float simulatedTime, float markerTime,
                                           float fixedStep, float* stepSize)
{
    constexpr float kMinimumStep = 1.0f / 500.0f;
    constexpr float kStepTolerance = 1.15f;
    *stepSize = fixedStep;
    if (!engineSettings().physicsFrameStep || !(fixedStep > 0.0f))
    {
        return -1;
    }
    const float startTime = simulatedTime > currentTime ? simulatedTime : currentTime;
    const float remaining = markerTime - startTime;
    if (!(remaining > kMinimumStep))
    {
        return 0;
    }
    int stepCount = (int)std::ceil(remaining / (fixedStep * kStepTolerance));
    if (stepCount < 1)
    {
        stepCount = 1;
    }
    *stepSize = remaining / (float)stepCount;
    return stepCount;
}
