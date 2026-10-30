/*
 * engine_settings.h -- the port's own settings (the in-game engine menu edits
 * them, folklore_settings.ini keeps them between runs).
 *
 * Adding a setting: a field here, a default in engine_settings.cpp's
 * defaults, a line in its load/save tables, and a widget in engine_menu.cpp.
 */
#pragma once

#include <cstdint>
#include <string>

enum class FrameRateCap : int
{
    Original = 0,       /* the game's own limiter: 30 fps (21 in its heavy mode) */
    Sixty = 1,          /* the limiter's targets scaled to 60 fps */
    Unlimited = 2,      /* limiter off; still paced by the 59.94 Hz flip */
};

enum class AspectRatio : int
{
    Original = 0,       /* 16:9, the PS3's */
    MatchDisplay = 1,   /* the main monitor's shape */
    Ultrawide = 2,      /* 21:9 (64:27, 2560x1080 / 3440x1440 class) */
    SuperUltrawide = 3, /* 32:9 */
};

enum class AntiAliasing : int
{
    Off = 0,
    Fxaa = 1,           /* FXAA over the presented image */
};

/* The console language the game is told (cellSysutilGetSystemParamInt,
 * CELL_SYSUTIL_SYSTEMPARAM_ID_LANG); the values are the system's own
 * CELL_SYSUTIL_LANG_* codes. The European release has text, subtitles and
 * the movie_996 video in these five and turns any other language into
 * English; the North American release only has English on the disc. */
enum class GameLanguage : int
{
    English = 1,
    French = 2,
    Spanish = 3,
    German = 4,
    Italian = 5,
};

struct EngineSettings
{
    FrameRateCap frameRateCap = FrameRateCap::Original;
    /* The limiter polls the clock between sleeps; true wakes it every 1 ms
     * instead of the game's 10 ms, so a frame ends on its target instead of
     * up to 10 ms after it. */
    bool fineLimiterSleep = true;
    bool showPerformanceOverlay = false;
    /* Graphics. renderScale: colour and depth buffers at 1-3x the PS3's
     * 1280x720; read when the renderer starts, so a change applies on the
     * next launch. The other two apply at once. */
    int renderScale = 1;
    AntiAliasing antiAliasing = AntiAliasing::Off;
    bool backgroundShaders = true;
    /* Anisotropic filtering: 1 (off), 2, 4, 8 or 16 -- sharper textures on
     * surfaces seen at an angle (floors, roads, walls). Sharpening: 0-100 %
     * contrast-adaptive sharpening of the final image. Both apply at once. */
    int anisotropy = 16;
    int sharpening = 0;
    /* Physics: step Havok by the real frame time instead of the game's fixed
     * 1/30 s (see folklore_physics_plan_steps). Keeps the cloth and hair
     * stable above 30 fps; at 30 fps it steps the same as the original. */
    bool physicsFrameStep = true;
    /* Start skips the opening logos (movie 995) as it skips every other
     * movie, after the first 40 frames (folklore_logo_movie_skippable). */
    bool skipLogos = true;
    /* Motion controls for capturing Folks, whatever the controller: on, the
     * gestures come from its motion sensors (DualShock 3 or 4, DualSense,
     * Switch Pro) and a controller without them still captures by itself;
     * off, every capture completes by itself (cellPad_set_motion_mode). */
    bool motionControls = true;
    /* Ultrawide: the game's field of view is widened to this shape. Read
     * when the renderer starts, so a change applies on the next launch. */
    AspectRatio aspectRatio = AspectRatio::Original;
    /* Borderless fullscreen on the window's monitor. Alt+Enter toggles it
     * too; the menu keeps this in step with the window. Applies at once. */
    bool fullscreen = false;
    /* The European release only (the North American one always gets
     * English). The game reads the language once, as it starts, so a
     * change applies on the next launch. */
    GameLanguage language = GameLanguage::English;
    /* Where things are (engine_portable.cpp). gameDir: the folder holding
     * PS3_GAME, with the decrypted PS3_GAME\USRDIR\EBOOT.elf; asked for with
     * a folder picker on the first start. devFlashDir: optional firmware
     * dev_flash (fonts). dataDir: where hdd0, hdd1 (several GB of installed
     * game data), gamedata (saves), the shader cache and logs go when the
     * exe runs on its own; empty = next to folklore.exe.
     * run.ps1's environment overrides all three. updateDir (1.10 builds
     * only): the installed 1.10 update, RPCS3's or a PS3's
     * dev_hdd0\game\<title id>, with the decrypted USRDIR\EBOOT.elf. */
    std::string gameDir;
    std::string updateDir;
    std::string devFlashDir;
    std::string dataDir;
};

EngineSettings& engineSettings();
void engineSettingsLoad();
void engineSettingsSave();
/* $FOLKLORE_SETTINGS, else folklore_settings.ini next to folklore.exe. */
const char* engineSettingsPath();
/* The folder folklore.exe is in, without a trailing separator. */
std::string engineExecutableDirectory();
/* Hand the graphics settings to the renderer (startup and every change). */
void engineSettingsApplyGraphics();
/* Hand the input settings to the pad runtime (startup and every change). */
void engineSettingsApplyInput();
/* The aspect ratio a setting asks for (16:9 for Original). */
float engineSettingsAspectValue(AspectRatio aspectRatio);
/* Whether this build's release has more than one language (the EU one). */
bool engineSettingsLanguageSelectable();
/* The language this run of the game was given (fixed once the game has
 * asked), for the menu to compare with the setting. */
GameLanguage engineSettingsActiveLanguage();
const char* engineSettingsLanguageName(GameLanguage language);

extern "C"
{
    /* Called from the lifted main loops (see tools/patch_limiter.py). */
    int folklore_limiter_target(int originalTarget);
    int folklore_limiter_sleep_us(int originalSleepMicroseconds);
    /* Called from the lifted Havok step helper func_003C913C (see
     * tools/game_patches.py, patch 0x3C913C). */
    int folklore_physics_plan_steps(float currentTime, float simulatedTime, float markerTime,
                                    float fixedStep, float* stepSize);
    /* The CELL_SYSUTIL_LANG_* value for the game's language query
     * (src/hle/folklore_hle.cpp). The first call fixes it for the run. */
    int folklore_system_language(void);
    /* Called from the lifted movie player (tools/game_patches.py, the
     * logoSkip patch): 1 when Start may skip the opening logos. */
    int folklore_logo_movie_skippable(void);
}
