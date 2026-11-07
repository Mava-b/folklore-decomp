/*
 * ps3recomp - cellAudio HLE implementation
 *
 * Real audio mixing and output. A background mixing thread reads audio data
 * from each active port's buffer in guest memory, mixes them, and outputs
 * to the host audio device.
 *
 * Backend selection:
 *   - Windows default: WASAPI (via mmdeviceapi)
 *   - Everywhere else / if PS3RECOMP_AUDIO_USE_SDL2 defined: SDL2 audio
 *
 * Define PS3RECOMP_AUDIO_USE_SDL2 to force SDL2 backend on Windows.
 */

#include "../../runtime/memory/vm.h"   /* vm_commit */
#include "cellAudio.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base, vm_read64, vm_write32 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <ctype.h>
#include "ps3emu/log_first.h"
#include "ps3emu/thread_name.h"

/* Event-queue push/lookup (runtime/syscalls/sys_event.c). Forward-declared
 * rather than including sys_event.h to avoid pulling PPU context/syscall
 * table types into the HLE audio module. */
extern int      sys_event_queue_push_by_id(uint32_t queue_id,
                                            uint64_t source, uint64_t data1,
                                            uint64_t data2,  uint64_t data3);
extern uint32_t sys_event_find_queue_by_key(uint64_t key);
extern uint32_t sys_event_queue_create_direct(uint64_t key, int32_t size);

/* ---------------------------------------------------------------------------
 * Backend selection
 * -----------------------------------------------------------------------*/

#if defined(PS3RECOMP_AUDIO_USE_SDL2)
  #define AUDIO_BACKEND_SDL2    1
  #define AUDIO_BACKEND_WASAPI  0
#elif defined(_WIN32)
  #define AUDIO_BACKEND_SDL2    0
  #define AUDIO_BACKEND_WASAPI  1
#else
  #define AUDIO_BACKEND_SDL2    1
  #define AUDIO_BACKEND_WASAPI  0
#endif

#if AUDIO_BACKEND_WASAPI
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <mmdeviceapi.h>
  #include <audioclient.h>
  #include <process.h>

  /* Define WASAPI COM GUIDs (avoids needing uuid.lib linkage for these) */
  #ifdef __cplusplus
    #define GUID_SECT
  #else
    #define GUID_SECT
  #endif
  #ifndef DEFINE_AUDIO_GUID
    #ifdef INITGUID
      #define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
              const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
    #else
      #define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
              const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
    #endif
  #endif
  DEFINE_AUDIO_GUID(ps3r_CLSID_MMDeviceEnumerator, 0xBCDE0395,0xE52F,0x467C,0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E);
  DEFINE_AUDIO_GUID(ps3r_IID_IMMDeviceEnumerator,  0xA95664D2,0x9614,0x4F35,0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioClient,         0x1CB9AD4C,0xDBFA,0x4c32,0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioRenderClient,   0xF294ACFC,0x3146,0x4483,0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2);

  /* Map to the names used in the code */
  #define IID_IMMDeviceEnumerator  ps3r_IID_IMMDeviceEnumerator
  #define CLSID_MMDeviceEnumerator ps3r_CLSID_MMDeviceEnumerator
  #define IID_IAudioClient         ps3r_IID_IAudioClient
  #define IID_IAudioRenderClient   ps3r_IID_IAudioRenderClient
#endif

#if AUDIO_BACKEND_SDL2
  #include <SDL2/SDL.h>
#endif

/* Portable threading */
#ifdef _WIN32
  #include <windows.h>
  typedef HANDLE thread_t;
  typedef CRITICAL_SECTION mutex_t;
  #define mutex_init(m)    InitializeCriticalSection(m)
  #define mutex_destroy(m) DeleteCriticalSection(m)
  #define mutex_lock(m)    EnterCriticalSection(m)
  #define mutex_unlock(m)  LeaveCriticalSection(m)
#else
  #include <pthread.h>
  #include <unistd.h>
  #include <time.h>
  typedef pthread_t thread_t;
  typedef pthread_mutex_t mutex_t;
  #define mutex_init(m)    pthread_mutex_init(m, NULL)
  #define mutex_destroy(m) pthread_mutex_destroy(m)
  #define mutex_lock(m)    pthread_mutex_lock(m)
  #define mutex_unlock(m)  pthread_mutex_unlock(m)
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

/* Per-port audio buffer allocated on the host side.
 * In a full emulator these would be in guest VM memory; here we allocate
 * host buffers and expose their addresses through portAddr/readIndexAddr. */
#define AUDIO_PORT_BUF_MAX  (CELL_AUDIO_BLOCK_32 * CELL_AUDIO_BLOCK_SAMPLES * CELL_AUDIO_PORT_8CH)

typedef struct {
    int                  in_use;
    int                  running;
    CellAudioPortParam   param;
    float*               buffer;        /* host audio buffer (float samples) */
    u32                  buf_size;       /* buffer size in bytes */
    u64                  read_index;     /* current read position (block index) */
    u64                  write_index;    /* where the game is writing */
    /* For address reporting to guest */
    u64                  port_addr;      /* guest-visible buffer address */
    u64                  read_idx_addr;  /* guest-visible read index address */
    int                  was_playing;    /* the last block mixed held sound */
    int                  ever_started;   /* PortStart was called at least once */
} AudioPortSlot;

static int            s_audio_initialized = 0;
static AudioPortSlot  s_ports[CELL_AUDIO_PORT_MAX];

/* Event queue notification */
typedef struct {
    int  in_use;
    u64  key;
} AudioNotifySlot;
static AudioNotifySlot s_notify_queues[CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES];

/* Timestamp anchor for cellAudioGetPortTimestamp (microseconds, set at init) */
static u64            s_audio_start_us = 0;

/* Mixing thread */
static unsigned long long g_audio_dropped;   /* frames the device had no room for */
#ifdef _WIN32
static unsigned g_audio_holes[CELL_AUDIO_PORT_MAX];   /* AUDIO_RATE: empty blocks right after a block with sound */
#endif
static volatile int  s_mix_thread_running = 0;
static thread_t      s_mix_thread;
#ifndef _WIN32
/* Is s_mix_thread a thread that exists and has not been joined? The Win32
 * branch asks that of the HANDLE itself, which it nulls after closing;
 * pthread_t has no such value, so carry the flag. */
static int           s_mix_thread_live = 0;
#endif
static mutex_t       s_audio_mutex;

/* Output mix buffer (stereo, one block worth) */
static float s_mix_buffer[CELL_AUDIO_BLOCK_SAMPLES * 2];

/* What went wrong with the sound since the last [cellAudio] report: blocks
 * where the device had played everything we gave it (a gap the listener
 * hears), blocks the game had not written in time (a port that was playing
 * sound handed us an empty block), and blocks the limiter turned down. */
static volatile unsigned s_audio_device_dry = 0;
static volatile unsigned s_audio_game_late = 0;
static volatile unsigned s_audio_limited_blocks = 0;
static float s_audio_deepest_gain = 1.0f;

/* ---------------------------------------------------------------------------
 * Host audio output backend
 * -----------------------------------------------------------------------*/

/* PS3_AUDIO_VOLUME=0..100: the host's master volume in percent (100 = the
 * title's own mix, unchanged). Applied to the final mix just before the
 * device, on a squared curve so the slider sounds even (50 is about -12 dB,
 * 25 about -24 dB); 0 is silence. Like PS3_AUDIO_MUTE it only touches what the
 * host plays, never what the title sees. */
static float audio_volume_gain(const char* e)
{
    if (!e || !*e) return 1.0f;
    char* end = NULL;
    double v = strtod(e, &end);
    if (end == e || v != v) return 1.0f;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return (float)((v / 100.0) * (v / 100.0));
}

/* The gain in use, file scope so the in-game menu (libs/video/ps3_overlay.mm,
 * from the inFAMOUS 2 port) can set the volume live: < 0 until
 * PS3_AUDIO_VOLUME is first read. */
static volatile float s_host_gain = -1.0f;
static volatile int   s_host_volume = -1;   /* percent, as the menu shows it */
static float cellAudio_host_gain(void)
{
    float gain = s_host_gain;
    if (gain < 0) {
        const char* e = getenv("PS3_AUDIO_VOLUME");
        gain = audio_volume_gain(e);
        if (gain != 1.0f) printf("[cellAudio] PS3_AUDIO_VOLUME=%s: gain %.3f\n", e, gain);
        s_host_volume = (int)(sqrt((double)gain) * 100.0 + 0.5);
        s_host_gain = gain;
    }
    return gain;
}
int cellAudio_host_volume(void) { cellAudio_host_gain(); return s_host_volume; }
void cellAudio_host_set_volume(int pct)
{
    char b[16];
    snprintf(b, sizeof b, "%d", pct < 0 ? 0 : pct > 100 ? 100 : pct);
    s_host_volume = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    s_host_gain = audio_volume_gain(b);
}

#if AUDIO_BACKEND_SDL2

static SDL_AudioDeviceID s_sdl_audio_dev = 0;

/* Case-insensitive strstr (ASCII), for audio_match_device. */
static const char* audio_find_nocase(const char* hay, const char* needle)
{
    for (; *hay; hay++) {
        size_t k = 0;
        while (needle[k] && hay[k] &&
               tolower((unsigned char)hay[k]) == tolower((unsigned char)needle[k])) k++;
        if (!needle[k]) return hay;
    }
    return NULL;
}

/* PS3_AUDIO_DEVICE=<name>: which output device the host plays to. The name
 * is matched exactly first, then ignoring case, then as a part of a device's
 * name ("AirPods" finds "Erik's AirPods Pro"); unset or no match = the
 * system default output (which SDL follows when it changes). SDL backend
 * only; WASAPI keeps the default endpoint. Returns the index of the device
 * in names[], or -1. */
static int audio_match_device(const char* want, const char* const* names, int n)
{
    if (!want || !*want) return -1;
    for (int i = 0; i < n; i++) if (names[i] && !strcmp(names[i], want)) return i;
    for (int i = 0; i < n; i++)
        if (names[i] && strlen(names[i]) == strlen(want) && audio_find_nocase(names[i], want) == names[i])
            return i;
    for (int i = 0; i < n; i++) if (names[i] && audio_find_nocase(names[i], want)) return i;
    return -1;
}

/* The SDL output device PS3_AUDIO_DEVICE names, or NULL for the default. */
static const char* audio_sdl_pick_device(void)
{
    static char picked[256];
    const char* want = getenv("PS3_AUDIO_DEVICE");
    if (!want || !*want) return NULL;
    const char* names[32];
    int n = SDL_GetNumAudioDevices(0);
    if (n > 32) n = 32;
    for (int i = 0; i < n; i++) names[i] = SDL_GetAudioDeviceName(i, 0);
    const int k = audio_match_device(want, names, n < 0 ? 0 : n);
    if (k < 0) {
        printf("[cellAudio] PS3_AUDIO_DEVICE=%s: no such output, using the default. Outputs:\n", want);
        for (int i = 0; i < n; i++) printf("[cellAudio]   %s\n", names[i] ? names[i] : "?");
        return NULL;
    }
    snprintf(picked, sizeof picked, "%s", names[k]);
    return picked;
}

static int audio_backend_init(void)
{
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            printf("[cellAudio] SDL audio init failed: %s\n", SDL_GetError());
            return -1;
        }
    }

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq     = CELL_AUDIO_SAMPLE_RATE;
    want.format   = AUDIO_F32SYS;
    want.channels = 2;
    want.samples  = CELL_AUDIO_BLOCK_SAMPLES;
    want.callback = NULL; /* We'll use SDL_QueueAudio */

    const char* dev = audio_sdl_pick_device();
    s_sdl_audio_dev = SDL_OpenAudioDevice(dev, 0, &want, &have, 0);
    if (s_sdl_audio_dev == 0 && dev) {
        printf("[cellAudio] cannot open \"%s\" (%s), using the default output\n", dev, SDL_GetError());
        dev = NULL;
        s_sdl_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    }
    if (s_sdl_audio_dev == 0) {
        printf("[cellAudio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return -1;
    }
    printf("[cellAudio] output: %s (%d Hz, %u ch, %u-frame device buffer)\n",
           dev ? dev : "system default", have.freq, (unsigned)have.channels,
           (unsigned)have.samples);

    SDL_PauseAudioDevice(s_sdl_audio_dev, 0); /* Start playback */
    return 0;
}

static void audio_backend_shutdown(void)
{
    if (s_sdl_audio_dev) {
        SDL_CloseAudioDevice(s_sdl_audio_dev);
        s_sdl_audio_dev = 0;
    }
}

static void audio_backend_submit(const float* stereo_samples, u32 num_samples)
{
    if (s_sdl_audio_dev) {
        SDL_QueueAudio(s_sdl_audio_dev, stereo_samples,
                       num_samples * 2 * sizeof(float));
    }
}

/* SDL queues without bound: keep about four blocks ahead of the device. */
static int audio_backend_room(void)
{
    if (!s_sdl_audio_dev) return -1;
    return SDL_GetQueuedAudioSize(s_sdl_audio_dev) / (2 * sizeof(float)) <
           CELL_AUDIO_BLOCK_SAMPLES * 4 ? CELL_AUDIO_BLOCK_SAMPLES : 0;
}

static void audio_backend_wait(void) { SDL_Delay(1); }

static u32 audio_backend_queued_samples(void)
{
    if (s_sdl_audio_dev) {
        return SDL_GetQueuedAudioSize(s_sdl_audio_dev) / (2 * sizeof(float));
    }
    return 0;
}

#endif /* AUDIO_BACKEND_SDL2 */

#if AUDIO_BACKEND_WASAPI

static IAudioClient*        s_wasapi_client = NULL;
static IAudioRenderClient*  s_wasapi_render = NULL;
static HANDLE               s_wasapi_event  = NULL;
static UINT32               s_wasapi_buf_frames = 0;

static int audio_backend_init(void)
{
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        printf("[cellAudio] CoInitializeEx failed: 0x%08lX\n", hr);
        return -1;
    }

    IMMDeviceEnumerator* enumerator = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void**)&enumerator);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to create device enumerator: 0x%08lX\n", hr);
        return -1;
    }

    IMMDevice* device = NULL;
    hr = enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender,
                                                      eConsole, &device);
    enumerator->lpVtbl->Release(enumerator);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get default audio endpoint: 0x%08lX\n", hr);
        return -1;
    }

    hr = device->lpVtbl->Activate(device, &IID_IAudioClient, CLSCTX_ALL,
                                   NULL, (void**)&s_wasapi_client);
    device->lpVtbl->Release(device);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to activate audio client: 0x%08lX\n", hr);
        return -1;
    }

    WAVEFORMATEX wfx;
    wfx.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = CELL_AUDIO_SAMPLE_RATE;
    wfx.wBitsPerSample  = 32;
    wfx.nBlockAlign     = wfx.nChannels * (wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize          = 0;

    /* Event-driven, with a 100 ms buffer by default (PS3_AUDIO_BUF_MS=<10..500>
     * sets it). The Windows mixer keeps only ~32 ms of it filled
     * (AUDIO_LATENCY_MS, audio_mix_thread_func), so the latency stays low; the
     * rest is room for the mixing thread to be late under a heavy frame without
     * the device running dry. (It was 20 ms: one late wake-up was a gap.) */
    REFERENCE_TIME buf_duration = 1000000; /* 100 ms in 100 ns units */
    { const char* e = getenv("PS3_AUDIO_BUF_MS"); const int ms = e ? atoi(e) : 0;
      if (ms >= 10 && ms <= 500) buf_duration = (REFERENCE_TIME)ms * 10000; }
    s_wasapi_event = CreateEventW(NULL, FALSE, FALSE, NULL);

    /* Shared mode takes only the device's own mix format unless Windows is
     * asked to convert: a device set to 44.1 or 96 kHz, or to 5.1 / 7.1,
     * refused this 48 kHz stereo stream with AUDCLNT_E_UNSUPPORTED_FORMAT
     * (0x88890008) and the game ran silent. AUTOCONVERTPCM has the audio
     * engine convert the rate and channel count; the attempts without it are
     * kept last for systems that do not accept the flag. */
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM      0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif
    const DWORD convertFlags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    const DWORD attemptFlags[4] = {
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | convertFlags,
        convertFlags,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        0,
    };
    int attempt = 0;
    hr = E_FAIL;
    for (attempt = 0; attempt < 4; attempt++) {
        hr = s_wasapi_client->lpVtbl->Initialize(
            s_wasapi_client,
            AUDCLNT_SHAREMODE_SHARED,
            attemptFlags[attempt],
            buf_duration, 0, &wfx, NULL);
        if (SUCCEEDED(hr)) {
            break;
        }
        if (hr != AUDCLNT_E_ALREADY_INITIALIZED) {
            continue;
        }
        break;
    }

    if (FAILED(hr)) {
        printf("[cellAudio] WASAPI Initialize failed: 0x%08lX\n", hr);
        /* What the device wanted instead, for the bug report. */
        WAVEFORMATEX* mixFormat = NULL;
        if (SUCCEEDED(s_wasapi_client->lpVtbl->GetMixFormat(s_wasapi_client, &mixFormat)) && mixFormat) {
            printf("[cellAudio] device mix format: %lu Hz, %u channels, %u bits, tag 0x%04X\n",
                   (unsigned long)mixFormat->nSamplesPerSec, (unsigned)mixFormat->nChannels,
                   (unsigned)mixFormat->wBitsPerSample, (unsigned)mixFormat->wFormatTag);
            CoTaskMemFree(mixFormat);
        }
        s_wasapi_client->lpVtbl->Release(s_wasapi_client);
        s_wasapi_client = NULL;
        CloseHandle(s_wasapi_event);
        s_wasapi_event = NULL;
        return -1;
    }
    if (attemptFlags[attempt] & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) {
        s_wasapi_client->lpVtbl->SetEventHandle(s_wasapi_client, s_wasapi_event);
    } else {
        CloseHandle(s_wasapi_event);
        s_wasapi_event = NULL;
    }
    if (attemptFlags[attempt] & AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM) {
        WAVEFORMATEX* mixFormat = NULL;
        if (SUCCEEDED(s_wasapi_client->lpVtbl->GetMixFormat(s_wasapi_client, &mixFormat)) && mixFormat) {
            printf("[cellAudio] WASAPI: 48000 Hz stereo, converted by Windows to the device's %lu Hz, %u channels\n",
                   (unsigned long)mixFormat->nSamplesPerSec, (unsigned)mixFormat->nChannels);
            CoTaskMemFree(mixFormat);
        }
    }

    s_wasapi_client->lpVtbl->GetBufferSize(s_wasapi_client, &s_wasapi_buf_frames);

    hr = s_wasapi_client->lpVtbl->GetService(
        s_wasapi_client, &IID_IAudioRenderClient, (void**)&s_wasapi_render);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get render client: 0x%08lX\n", hr);
        s_wasapi_client->lpVtbl->Release(s_wasapi_client);
        s_wasapi_client = NULL;
        return -1;
    }

    s_wasapi_client->lpVtbl->Start(s_wasapi_client);
    return 0;
}

static void audio_backend_shutdown(void)
{
    if (s_wasapi_client) {
        s_wasapi_client->lpVtbl->Stop(s_wasapi_client);
    }
    if (s_wasapi_render) {
        s_wasapi_render->lpVtbl->Release(s_wasapi_render);
        s_wasapi_render = NULL;
    }
    if (s_wasapi_client) {
        s_wasapi_client->lpVtbl->Release(s_wasapi_client);
        s_wasapi_client = NULL;
    }
    if (s_wasapi_event) {
        CloseHandle(s_wasapi_event);
        s_wasapi_event = NULL;
    }
}

static void audio_backend_submit(const float* stereo_samples, u32 num_samples)
{
    if (!s_wasapi_render) return;

    UINT32 padding = 0;
    s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);

    UINT32 available = s_wasapi_buf_frames - padding;
    if (num_samples > available) {
        g_audio_dropped += num_samples - available;
        num_samples = available;
    }
    if (num_samples == 0) return;

    BYTE* buf = NULL;
    HRESULT hr = s_wasapi_render->lpVtbl->GetBuffer(s_wasapi_render, num_samples, &buf);
    if (SUCCEEDED(hr) && buf) {
        /* PS3_MUTE=1: silence at the device, nothing else changes -- the
         * title still gets a working cellAudio and the device still paces
         * it, unlike PS3_NO_AUDIO, which sends the title down its no-audio
         * path. For unattended runs. */
        static int mute = -1;
        if (mute < 0) mute = getenv("PS3_MUTE") ? 1 : 0;
        memcpy(buf, stereo_samples, num_samples * 2 * sizeof(float));
        s_wasapi_render->lpVtbl->ReleaseBuffer(s_wasapi_render, num_samples,
                                               mute ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
    }
}

/* Frames the device can take right now; -1 with no device open. */
static int audio_backend_room(void)
{
    if (!s_wasapi_client) return -1;
    UINT32 padding = 0;
    const HRESULT hr = s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);
    if (FAILED(hr)) {
        /* Device gone (AUDCLNT_E_DEVICE_INVALIDATED on a default-device
         * change, sleep, ...): pace on the clock rather than spin. */
        static int _n = 0; if (PS3_LOG_FIRST(_n, 4))
            fprintf(stderr, "[cellAudio] GetCurrentPadding failed 0x%08lX -- clock pacing%c", (unsigned long)hr, 10);
        return -1;
    }
    return (int)(s_wasapi_buf_frames - padding);
}

/* Block until the device has consumed some of its buffer. */
static void audio_backend_wait(void)
{
    if (s_wasapi_event) WaitForSingleObject(s_wasapi_event, 10);
    else Sleep(1);
}


#endif /* AUDIO_BACKEND_WASAPI */

/* ---------------------------------------------------------------------------
 * Mixing
 * -----------------------------------------------------------------------*/

/* Guest PCM is BIG-ENDIAN float32; the port buffer is a raw vm_base view, so
 * every sample must be byte-swapped before use. Reading it as a host float
 * turned the whole mix into denormal noise -- silence after clipping (LBP
 * had a verified end-to-end pipeline with no audible output). */
static inline float ld_be_f32(const float* p)
{
    u32 v;
    memcpy(&v, p, 4);
    v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

/* PS3_AUDIO_DUMP=<file.wav>: the final mix (before PS3_AUDIO_MUTE and
 * PS3_AUDIO_VOLUME, so a muted run still records what would have played) as
 * a 48 kHz stereo 32-bit float WAV. The header's sizes are rewritten about
 * once a second, so a run that is killed leaves a file that still opens. */
static FILE* s_dump_wav = (FILE*)-1;
static u64   s_dump_frames;

static void audio_wav_header(unsigned char h[58], u64 frames)
{
    const u32 data = frames * 8u > 0xFFFFFF00ull ? 0xFFFFFF00u : (u32)(frames * 8u);
    const u32 v[] = { 0x46464952u /* RIFF */, 50u + data, 0x45564157u /* WAVE */,
                      0x20746D66u /* fmt  */, 18u };
    memcpy(h, v, sizeof v);                        /* host order: little-endian */
    const u16 fmt[] = { 3 /* IEEE float */, 2 };
    memcpy(h + 20, fmt, 4);
    const u32 rate[] = { CELL_AUDIO_SAMPLE_RATE, CELL_AUDIO_SAMPLE_RATE * 8u };
    memcpy(h + 24, rate, 8);
    const u16 blk[] = { 8, 32, 0 };
    memcpy(h + 32, blk, 6);
    const u32 fact[] = { 0x74636166u /* fact */, 4u, (u32)(data / 8u),
                         0x61746164u /* data */, data };
    memcpy(h + 38, fact, 20);
}

/* Opened by cellAudioInit. The first open in a run truncates; a later one --
 * cellAudioInit after cellAudioQuit, or the process a title exitspawns into,
 * which inherits PS3_AUDIO_DUMP_CONTINUE -- appends, so one file holds the
 * whole session (Resistance's EBOOT and its game.self, for instance). */
static void audio_dump_open(void)
{
    if (s_dump_wav != (FILE*)-1) return;
    s_dump_wav = NULL;
    s_dump_frames = 0;
    const char* e = getenv("PS3_AUDIO_DUMP");
    if (!e || !*e) return;
    if (getenv("PS3_AUDIO_DUMP_CONTINUE")) {
        FILE* f = fopen(e, "r+b");
        unsigned char h[58], want[58];
        audio_wav_header(want, 0);
        if (f && fread(h, 1, sizeof h, f) == sizeof h &&
            !memcmp(h, want, 4) && !memcmp(h + 8, want + 8, 30)) {
            fseek(f, 0, SEEK_END);
            const long end = ftell(f);
            s_dump_frames = end > 58 ? (u64)(end - 58) / 8u : 0;
            s_dump_wav = f;
            printf("[cellAudio] PS3_AUDIO_DUMP=%s: appending at %.1f s\n", e,
                   s_dump_frames / (double)CELL_AUDIO_SAMPLE_RATE);
            return;
        }
        if (f) fclose(f);
    }
    s_dump_wav = fopen(e, "wb");
    if (!s_dump_wav) return;
    unsigned char h[58];
    audio_wav_header(h, 0);
    fwrite(h, 1, sizeof h, s_dump_wav);
    fflush(s_dump_wav);
#ifdef _WIN32
    _putenv_s("PS3_AUDIO_DUMP_CONTINUE", "1");
#else
    setenv("PS3_AUDIO_DUMP_CONTINUE", "1", 1);
#endif
    printf("[cellAudio] PS3_AUDIO_DUMP=%s: recording the mix\n", e);
}

static void audio_dump_block(const float* stereo, u32 frames)
{
    if (!s_dump_wav || s_dump_wav == (FILE*)-1) return;
    fwrite(stereo, sizeof(float), (size_t)frames * 2, s_dump_wav);
    s_dump_frames += frames;
    if ((s_dump_frames % (CELL_AUDIO_BLOCK_SAMPLES * 188u)) < frames) {
        unsigned char h[58];
        audio_wav_header(h, s_dump_frames);
        fseek(s_dump_wav, 0, SEEK_SET);
        fwrite(h, 1, sizeof h, s_dump_wav);
        fseek(s_dump_wav, 0, SEEK_END);
        fflush(s_dump_wav);
    }
}

static void audio_dump_close(void)
{
    if (!s_dump_wav || s_dump_wav == (FILE*)-1) return;
    unsigned char h[58];
    audio_wav_header(h, s_dump_frames);
    fseek(s_dump_wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, s_dump_wav);
    fclose(s_dump_wav);
    s_dump_wav = (FILE*)-1;
    s_dump_frames = 0;
}

/* PS3_AUDIO_STATS=1: every ~5 s, the level of each running port and of the
 * mix (RMS / peak over the interval, the share of blocks that were all zero,
 * and how many mixed samples had to be clipped), so "which part is silent"
 * and "is it clipping" can be read from the log. */
typedef struct {
    double sq;
    float  peak;
    u32    blocks, silent;
    double ch_sq[8];   /* per channel, for an 8-channel port */
} AudioLevel;
static AudioLevel s_lvl_port[CELL_AUDIO_PORT_MAX], s_lvl_mix;
/* Blocks handed to the device since cellAudioInit, continued across an
 * exitspawn with the dump: the port lines below say where on the audio clock
 * (and so where in PS3_AUDIO_DUMP's file) they happened. */
static volatile u64 s_mixed_blocks;
static double audio_clock_s(void)
{
    const u64 frames = (s_dump_wav && s_dump_wav != (FILE*)-1)
                       ? s_dump_frames : s_mixed_blocks * CELL_AUDIO_BLOCK_SAMPLES;
    return (double)frames / CELL_AUDIO_SAMPLE_RATE;
}
static u64 s_lvl_clipped;
static int s_stats = -1;

static void audio_level_add(AudioLevel* l, const float* s, u32 n, int be, u32 nch)
{
    double sq = 0.0;
    float pk = 0.0f;
    for (u32 i = 0; i < n; i++) {
        const float v = be ? ld_be_f32(&s[i]) : s[i];
        sq += (double)v * v;
        if (nch == 8) l->ch_sq[i & 7] += (double)v * v / (n / 8);
        const float a = v < 0 ? -v : v;
        if (a > pk) pk = a;
    }
    l->sq += sq / (n ? n : 1);
    if (pk > l->peak) l->peak = pk;
    l->blocks++;
    if (pk == 0.0f) l->silent++;
}

static void audio_level_print(const char* name, int idx, AudioLevel* l)
{
    if (!l->blocks) return;
    const double rms = sqrt(l->sq / l->blocks);
    char tag[16];
    if (idx >= 0) snprintf(tag, sizeof tag, "%s %d", name, idx);
    else snprintf(tag, sizeof tag, "%s", name);
    printf("[audio-stats] %-6s rms %.4f (%6.1f dBFS) peak %.4f silent %5.1f%%",
           tag, rms, rms > 0 ? 20.0 * log10(rms) : -999.0, l->peak,
           100.0 * l->silent / l->blocks);
    if (l->ch_sq[0] + l->ch_sq[1] + l->ch_sq[2] + l->ch_sq[3] > 0.0) {
        static const char* nm[8] = { "L", "R", "C", "LFE", "Ls", "Rs", "Lb", "Rb" };
        printf(" |");
        for (int c = 0; c < 8; c++) printf(" %s %.4f", nm[c], sqrt(l->ch_sq[c] / l->blocks));
    }
    printf("\n");
    memset(l, 0, sizeof(*l));
}

static void audio_stats_tick(void)
{
    if (s_stats < 0) s_stats = getenv("PS3_AUDIO_STATS") ? 1 : 0;
    if (!s_stats) return;
    if ((s_mixed_blocks % 938u) != 0) return;     /* 938 blocks = 5.0 s */
    printf("[audio-stats] t=%.1fs (audio clock)\n", audio_clock_s());
    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) audio_level_print("port", p, &s_lvl_port[p]);
    audio_level_print("mix", -1, &s_lvl_mix);
    if (s_lvl_clipped) printf("[audio-stats] clipped %llu samples\n", (unsigned long long)s_lvl_clipped);
    s_lvl_clipped = 0;
    fflush(stdout);
}

/* Hand one mixed block to the host device. Shared by the Windows and POSIX
 * mixer loops so the debug taps behave the same on both:
 *   PS3_AUDIO_DUMP=<file.wav>  the final mix as a float WAV (above)
 *   AUDIO_WAV=<file>  append the final mix as raw f32le stereo 48 kHz
 *                     (ffmpeg -f f32le -ar 48000 -ac 2 -i <file>)
 *   PS3_AUDIO_MUTE=1  silence the HOST output only; the title still sees a
 *                     working device and every mix/pacing path runs as normal
 *                     (unlike PS3_NO_AUDIO, which changes what the guest does).
 *                     Both dumps still record the real mix.
 *   PS3_AUDIO_VOLUME  0..100, the host's master volume (audio_volume_gain);
 *                     the dumps record the mix before it too. */
static void audio_submit_mix(void)
{
    s_mixed_blocks++;
    audio_dump_block(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
    if (s_stats > 0) audio_level_add(&s_lvl_mix, s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES * 2, 0, 2);
    audio_stats_tick();
    { static FILE* _wf = (FILE*)-1;
      if (_wf == (FILE*)-1) { const char* e = getenv("AUDIO_WAV"); _wf = e ? fopen(e, "wb") : NULL; }
      if (_wf) { fwrite(s_mix_buffer, sizeof(float), CELL_AUDIO_BLOCK_SAMPLES * 2, _wf); fflush(_wf); } }
    { static int mute = -1; if (mute < 0) mute = getenv("PS3_AUDIO_MUTE") ? 1 : 0;
      if (mute) memset(s_mix_buffer, 0, sizeof(s_mix_buffer)); }
    { const float gain = cellAudio_host_gain();
      if (gain != 1.0f)
          for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) s_mix_buffer[i] *= gain; }
    audio_backend_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
}

/* Add one block of a port (nch big-endian floats a frame: 2, or 8 in the
 * cellAudio order L R C LFE Ls Rs Lb Rb) to a stereo host mix, at `level`.
 *
 * The 8-channel fold is RPCS3's (Emu/Audio/AudioBackend.h downmix, 7.1 ->
 * stereo): centre at -3 dB into both sides, each side's surround and back at
 * -3 dB into it, LFE left out -- the usual stereo downmix. It was centre
 * -3 dB, LFE -6 dB, surround -6 dB and back -10 dB into both, with the LFE
 * summed into each side: the coefficients added up to 3x full scale where
 * RPCS3's add up to 2.4x, and a movie's 5.1 bed (Resistance's are AC3 3/2 +
 * LFE) came out bass-heavy and clipping on the louder passages. */
static void audio_downmix_add(float* mix, const float* src, u32 nch, float level)
{
    const float k = 0.70710678f;
    for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
        float left, right;
        if (nch >= 2) {
            left  = ld_be_f32(&src[s * nch + 0]);
            right = ld_be_f32(&src[s * nch + 1]);
        } else {
            /* Mono: duplicate to both channels */
            left = right = ld_be_f32(&src[s]);
        }
        if (nch == 8) {
            const float center = ld_be_f32(&src[s * 8 + 2]) * k;
            left  += center + (ld_be_f32(&src[s * 8 + 4]) + ld_be_f32(&src[s * 8 + 6])) * k;
            right += center + (ld_be_f32(&src[s * 8 + 5]) + ld_be_f32(&src[s * 8 + 7])) * k;
        }
        mix[s * 2 + 0] += left * level;
        mix[s * 2 + 1] += right * level;
    }
}

static int audio_block_is_zero(const float* src, u32 nch)
{
    const u32* w = (const u32*)src;
    for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * nch; i++)
        if (w[i] & 0x7FFFFFFFu) return 0;
    return 1;
}

#ifndef _WIN32
/* A running port that was playing and whose next block is still empty: its
 * producer has not written it yet. */
static int audio_port_starved(void)
{
    int starved = 0;
    mutex_lock(&s_audio_mutex);
    for (int p = 0; p < CELL_AUDIO_PORT_MAX && !starved; p++) {
        const AudioPortSlot* port = &s_ports[p];
        if (!port->in_use || !port->running || !port->buffer || !port->was_playing) continue;
        const u32 nch = (u32)port->param.nChannel, nblock = (u32)port->param.nBlock;
        const float* src = port->buffer + (u32)(port->read_index % nblock) * CELL_AUDIO_BLOCK_SAMPLES * nch;
        starved = audio_block_is_zero(src, nch);
    }
    mutex_unlock(&s_audio_mutex);
    return starved;
}
#endif

/* Peak limiter for the final stereo mix.
 *
 * The ports are summed (and a 7.1 port folded down to stereo) in float, so
 * loud moments -- two captures at once, a boss -- go past full scale. Those
 * samples used to be clipped flat at +-1.0, which is the "crunchy" sound.
 * This turns the whole mix down just enough instead: 1 ms look-ahead, a
 * fast attack and an 80 ms release, so peaks are caught before they arrive
 * and the gain comes back smoothly. Below the threshold the gain is exactly
 * 1 and the mix passes through unchanged (one millisecond later). A final
 * clamp catches the little the attack can leave over. AUDIO_NO_LIMITER=1
 * restores the plain clip. */
#define AUDIO_LIMIT_LOOKAHEAD 48
#define AUDIO_LIMIT_THRESHOLD 0.97f
static void audio_limit_block(float* stereo)
{
    static int disabled = -1;
    static float delayLine[AUDIO_LIMIT_LOOKAHEAD][2];
    static float needLine[AUDIO_LIMIT_LOOKAHEAD];
    static u32 position = 0;
    static float gain = 1.0f;
    static float attackCoefficient = 0.0f;
    static float releaseCoefficient = 0.0f;
    if (disabled < 0)
    {
        const char* setting = getenv("AUDIO_NO_LIMITER");
        disabled = setting && setting[0] && setting[0] != '0';
        for (u32 index = 0; index < AUDIO_LIMIT_LOOKAHEAD; index++)
        {
            needLine[index] = 1.0f;
        }
        attackCoefficient = expf(-1.0f / 12.0f);
        releaseCoefficient = expf(-1.0f / (0.080f * (float)CELL_AUDIO_SAMPLE_RATE));
    }
    float lowestGain = 1.0f;
    for (u32 sample = 0; sample < CELL_AUDIO_BLOCK_SAMPLES; sample++)
    {
        float left = stereo[sample * 2 + 0];
        float right = stereo[sample * 2 + 1];
        if (!disabled)
        {
            const float peak = fmaxf(fabsf(left), fabsf(right));
            const float need = peak > AUDIO_LIMIT_THRESHOLD ? AUDIO_LIMIT_THRESHOLD / peak : 1.0f;
            /* The gain the output sample (the oldest in the delay line) and
             * everything after it up to this input need. */
            float windowNeed = need;
            for (u32 index = 0; index < AUDIO_LIMIT_LOOKAHEAD; index++)
            {
                if (needLine[index] < windowNeed)
                {
                    windowNeed = needLine[index];
                }
            }
            const float delayedLeft = delayLine[position][0];
            const float delayedRight = delayLine[position][1];
            delayLine[position][0] = left;
            delayLine[position][1] = right;
            needLine[position] = need;
            position = (position + 1u) % AUDIO_LIMIT_LOOKAHEAD;
            const float coefficient = windowNeed < gain ? attackCoefficient : releaseCoefficient;
            gain = windowNeed + (gain - windowNeed) * coefficient;
            if (gain < lowestGain)
            {
                lowestGain = gain;
            }
            left = delayedLeft * gain;
            right = delayedRight * gain;
        }
        if (left > 1.0f || left < -1.0f) s_lvl_clipped++;     /* PS3_AUDIO_STATS */
        if (right > 1.0f || right < -1.0f) s_lvl_clipped++;
        stereo[sample * 2 + 0] = left > 1.0f ? 1.0f : (left < -1.0f ? -1.0f : left);
        stereo[sample * 2 + 1] = right > 1.0f ? 1.0f : (right < -1.0f ? -1.0f : right);
    }
    if (lowestGain < 0.999f)
    {
        s_audio_limited_blocks++;
        if (lowestGain < s_audio_deepest_gain)
        {
            s_audio_deepest_gain = lowestGain;
        }
    }
}

/* Mix one block from all active ports into s_mix_buffer (stereo float) */
static void audio_mix_one_block(void)
{
    memset(s_mix_buffer, 0, sizeof(s_mix_buffer));

    mutex_lock(&s_audio_mutex);

    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
        /* Diagnostic: a port that HOLDS PCM but was never started is inaudible
         * by design -- flag it once so "no PortStart" vs "no data" is decidable
         * from the log (LBP's Bink movie audio port sat exactly there). */
        if (port->in_use && !port->running && !port->ever_started && port->buffer) {
            static u8 warned[CELL_AUDIO_PORT_MAX];
            if (!warned[p]) {
                u32 probe = (u32)(port->param.nBlock * CELL_AUDIO_BLOCK_SAMPLES *
                                  port->param.nChannel);
                int nz = 0;
                for (u32 s = 0; s < probe; s += 64) if (port->buffer[s] != 0.0f) { nz = 1; break; }
                if (nz) {
                    warned[p] = 1;
                    fprintf(stderr, "[cellAudio] port %d HAS DATA but never started"
                            " -- game withheld PortStart\n", p);
                }
            }
        }
        if (!port->in_use || !port->running || !port->buffer)
            continue;

        u32 nch    = (u32)port->param.nChannel;
        u32 nblock = (u32)port->param.nBlock;
        /* A playing port whose next block is still all zero (the firmware
         * clears each block it consumes, so zero means not rewritten) right
         * after a block with sound: the game's mixer was late. */
        {
            static int portHadSound[CELL_AUDIO_PORT_MAX];
            const u32 peekIndex = (u32)(port->read_index % nblock);
            const u32* peekWords = (const u32*)(port->buffer + peekIndex * CELL_AUDIO_BLOCK_SAMPLES * nch);
            int blockHasSound = 0;
            for (u32 word = 0; word < CELL_AUDIO_BLOCK_SAMPLES * nch; word++)
            {
                if (peekWords[word] & 0x7FFFFFFFu)
                {
                    blockHasSound = 1;
                    break;
                }
            }
            if (!blockHasSound && portHadSound[p])
            {
                s_audio_game_late++;
            }
            portHadSound[p] = blockHasSound;
        }
        float level = port->param.level;
        if (!(level >= 0.0f)) level = 0.0f;
        if (level > 1.0f) level = 1.0f;

        /* Read one block at the current read_index */
        u32 block_idx = (u32)(port->read_index % nblock);
        u32 block_offset = block_idx * CELL_AUDIO_BLOCK_SAMPLES * nch;
        float* src = port->buffer + block_offset;

        /* AUDIO_PEAK diag: which blocks of this port's WHOLE ring hold data,
         * vs which block the read cursor is on -- distinguishes "writer never
         * writes" from "writer and reader cycle out of phase". */
        { static int s_pk = -1; if (s_pk < 0) s_pk = getenv("AUDIO_PEAK") ? 1 : 0;
          if (s_pk) { static _Thread_local unsigned _c;
            if ((++_c % 400) == 0) {
                char map[36]; u32 b;
                for (b = 0; b < nblock && b < 32; b++) {
                    const float* bp = port->buffer + b * CELL_AUDIO_BLOCK_SAMPLES * nch;
                    int nz = 0;
                    for (u32 s2 = 0; s2 < CELL_AUDIO_BLOCK_SAMPLES * nch; s2 += 16)
                        if (bp[s2] != 0.0f) { nz = 1; break; }
                    map[b] = nz ? '#' : '.';
                }
                map[b] = 0;
                fprintf(stderr, "[audio-ring] port %d ridx=%llu blk=%u ring=[%s]\n",
                        p, (unsigned long long)port->read_index, block_idx, map);
            } } }

#ifdef _WIN32   /* reported by the Windows mix thread's AUDIO_RATE */
        /* AUDIO_RATE: a block with nothing in it right after one with sound is a hole, the guest's audio engine late
         * with it (what the mix then plays is a click or a gap, whatever the device does). */
        { static int hole_on = -1; static u32 prev_nz[CELL_AUDIO_PORT_MAX];
          if (hole_on < 0) hole_on = getenv("AUDIO_RATE") ? 1 : 0;
          if (hole_on) { int nz = 0;
            for (u32 s2 = 0; s2 < CELL_AUDIO_BLOCK_SAMPLES * nch; s2 += 2) if (src[s2] != 0.0f) { nz = 1; break; }
            if (!nz && prev_nz[p]) g_audio_holes[p]++;
            prev_nz[p] = nz; } }
#endif
        if (s_stats > 0)
            audio_level_add(&s_lvl_port[p], src, CELL_AUDIO_BLOCK_SAMPLES * nch, 1, nch);
        port->was_playing = !audio_block_is_zero(src, nch);

        audio_downmix_add(s_mix_buffer, src, nch, level);

        /* Firmware clears a ring-buffer block after consuming it. Besides
         * preventing stale PCM from replaying when a producer stalls, the
         * cleared block is an observable consumption signal for producers. */
        memset(src, 0, CELL_AUDIO_BLOCK_SAMPLES * nch * sizeof(float));

        /* Keep the monotonic counter for tags/timestamps, but publish the
         * ring slot to the guest. SurMixer multiplies this value by the block
         * size directly; an unbounded counter walks beyond the audio buffer. */
        port->read_index++;
        if (port->read_idx_addr)
            vm_write64((u32)port->read_idx_addr, port->read_index % nblock);
    }

    mutex_unlock(&s_audio_mutex);

    audio_limit_block(s_mix_buffer);
}

/* ---------------------------------------------------------------------------
 * Mixing thread
 * -----------------------------------------------------------------------*/

static void audio_notify_event_queues(void)
{
    /* Push the audio-period event (data1 = CELL_AUDIO_EVENT_MIX = 0) to each
     * registered notify queue, so the game's audio loop (blocked on
     * sys_event_queue_receive) wakes once per block and writes the next one.
     * Resolve the queue by its ipc_key (set via cellAudioSetNotifyEventQueue). */
    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) continue;
        uint32_t qid = sys_event_find_queue_by_key(s_notify_queues[i].key);
        if (qid)
            sys_event_queue_push_by_id(qid, s_notify_queues[i].key,
                                       0 /*CELL_AUDIO_EVENT_MIX*/, 0, 0);
    }
}

/* The loudest sample of the block just mixed (AUDIO_PEAK). */
static float audio_mix_peak(void)
{
    float pk = 0.0f;
    for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) {
        const float a = s_mix_buffer[i] < 0 ? -s_mix_buffer[i] : s_mix_buffer[i];
        if (a > pk) pk = a;
    }
    return pk;
}

#ifdef _WIN32
/* How far ahead of the device the mixer stays, in frames: AUDIO_LATENCY_MS
 * (default 32 ms, 6 blocks), kept within the device buffer. */
static int audio_target_frames(void)
{
    int milliseconds = 32;
    const char* setting = getenv("AUDIO_LATENCY_MS");
    if (setting && setting[0])
    {
        milliseconds = atoi(setting);
    }
    int frames = milliseconds * (CELL_AUDIO_SAMPLE_RATE / 1000);
    if (frames < CELL_AUDIO_BLOCK_SAMPLES * 2)
    {
        frames = CELL_AUDIO_BLOCK_SAMPLES * 2;
    }
#if AUDIO_BACKEND_WASAPI
    if (s_wasapi_buf_frames && frames > (int)s_wasapi_buf_frames - CELL_AUDIO_BLOCK_SAMPLES)
    {
        frames = (int)s_wasapi_buf_frames - CELL_AUDIO_BLOCK_SAMPLES;
    }
#endif
    return frames;
}

/* A millisecond: the pacing needs finer steps than the device's 10 ms
 * period event (the process runs with a 1 ms timer resolution). */
static void audio_backend_wait_short(void)
{
#if AUDIO_BACKEND_WASAPI
    if (s_wasapi_event)
    {
        WaitForSingleObject(s_wasapi_event, 1);
        return;
    }
#endif
    Sleep(1);
}

/* One line every 10 s, only when something was wrong. */
static void audio_report_problems(void)
{
    const unsigned dry = s_audio_device_dry;
    const unsigned late = s_audio_game_late;
    const unsigned limited = s_audio_limited_blocks;
    if (dry || late || limited)
    {
        fprintf(stderr, "[cellAudio] last 10 s: device ran dry %u times, game late with %u blocks, "
                        "limiter turned %u blocks down (deepest %.1f dB)\n",
                dry, late, limited, 20.0 * log10((double)(s_audio_deepest_gain > 1e-6f ? s_audio_deepest_gain : 1e-6f)));
    }
    s_audio_device_dry = 0;
    s_audio_game_late = 0;
    s_audio_limited_blocks = 0;
    s_audio_deepest_gain = 1.0f;
}

static unsigned long long g_audio_underruns;
static unsigned __stdcall audio_mix_thread_func(void* arg)
{
    (void)arg;
    ps3_thread_name("audio mix");
    printf("[cellAudio] Mixing thread started\n");

    /* The device deadline is every few ms and the guest threads keep every
     * core busy: run this one ahead of them (MMCSS "Pro Audio" when the
     * service is there, the highest normal priority anyway). avrt.dll is
     * loaded by hand so the runtime does not need avrt.lib to link. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    { HMODULE avrt = LoadLibraryW(L"avrt.dll");
      typedef HANDLE (WINAPI *set_mm_fn)(LPCWSTR, LPDWORD);
      set_mm_fn set_mm = avrt ? (set_mm_fn)(void*)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW") : NULL;
      DWORD task = 0;
      if (set_mm && !set_mm(L"Pro Audio", &task))
          fprintf(stderr, "[cellAudio] MMCSS Pro Audio unavailable (%lu)%c", (unsigned long)GetLastError(), 10); }

    /* Pace to the device clock. The PS3 consumes one block every 5.33 ms;
     * the old Sleep(2)/Sleep(5) loop keyed on a queue depth the 1056-frame
     * WASAPI buffer can never reach, so it mixed ~225 blocks/s and the device
     * dropped the surplus: 20% of the audio thrown away, and a guest mixer
     * (GH3's FMOD) that could not keep ahead of the read index played gaps.
     * With no device, pace on the performance counter instead. */
    LARGE_INTEGER qf, q0; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
    unsigned long long blocks = 0;
    /* Keep the device ~32 ms ahead (AUDIO_LATENCY_MS), and never take the
     * game's blocks faster than 1.25x real time. The PS3 reads one block
     * every 5.33 ms, and the game's mixer writes just ahead of that read
     * index; after a late wake-up this thread used to refill the whole
     * device buffer in one burst, reading blocks the game had not written
     * yet -- gaps, heard as crackle in busy scenes. Now the cushion absorbs
     * the late wake-up and refills at a pace the game keeps up with. */
    const int targetFrames = audio_target_frames();
    const LONGLONG blockTicks = qf.QuadPart * CELL_AUDIO_BLOCK_SAMPLES / CELL_AUDIO_SAMPLE_RATE;
    const LONGLONG minimumSpacing = blockTicks * 4 / 5;
    LARGE_INTEGER lastMix; lastMix.QuadPart = 0;
    LARGE_INTEGER lastReport; QueryPerformanceCounter(&lastReport);
    while (s_mix_thread_running) {
        const int room = audio_backend_room();
        if (room >= 0) {
#if AUDIO_BACKEND_WASAPI
            const int queued = (int)s_wasapi_buf_frames - room;
#else
            const int queued = 0;
#endif
            if (room < CELL_AUDIO_BLOCK_SAMPLES || queued >= targetFrames) {
                audio_backend_wait_short();
                continue;
            }
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            if (queued >= CELL_AUDIO_BLOCK_SAMPLES && now.QuadPart - lastMix.QuadPart < minimumSpacing) {
                audio_backend_wait_short();
                continue;
            }
            if (queued == 0 && blocks > (unsigned long long)(targetFrames / CELL_AUDIO_BLOCK_SAMPLES) + 4u) {
                s_audio_device_dry++;
            }
#if AUDIO_BACKEND_WASAPI
            /* Device queue under half a block: it is about to play silence (an
             * audible click/stutter). AUDIO_RATE reports how often. */
            if (blocks > 64 && queued < CELL_AUDIO_BLOCK_SAMPLES / 2) g_audio_underruns++;
#endif
            lastMix = now;
            if (now.QuadPart - lastReport.QuadPart >= qf.QuadPart * 10) {
                audio_report_problems();
                lastReport = now;
            }
        }
        if (room < 0) {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            const long long due = q0.QuadPart +
                (long long)(blocks * CELL_AUDIO_BLOCK_SAMPLES * qf.QuadPart / CELL_AUDIO_SAMPLE_RATE);
            if (now.QuadPart < due) { Sleep(1); continue; }
            /* More than 50 ms behind (device just went away, a debugger
             * stop): drop the backlog instead of mixing it in a burst. */
            if (now.QuadPart - due > qf.QuadPart / 20)
                q0.QuadPart = now.QuadPart -
                    (long long)(blocks * CELL_AUDIO_BLOCK_SAMPLES * qf.QuadPart / CELL_AUDIO_SAMPLE_RATE);
        }
        blocks++;
        /* Mix and submit one block */
        audio_mix_one_block();
        /* AUDIO_PEAK=1: report the mixed block's peak amplitude periodically so
         * "is any port producing sound" is answerable (LBP Bink movie audio). */
        { static int _ap = -1; if (_ap < 0) _ap = getenv("AUDIO_PEAK") ? 1 : 0;
          if (_ap) { static unsigned _n = 0; const float pk = audio_mix_peak();
            if ((++_n % 200) == 0 || (pk > 0.001f && _n < 40))
                fprintf(stderr, "[audio-peak] block#%u peak=%.4f\n", _n, pk); } }
        audio_submit_mix();
        /* AUDIO_RATE=1: blocks mixed vs real time, and frames dropped. */
        { static int on = -1; if (on < 0) on = getenv("AUDIO_RATE") ? 1 : 0;
          if (on) { static ULONGLONG t0; static unsigned n, z; static unsigned long long d0, u0;
            ULONGLONG now = GetTickCount64(); if (!t0) t0 = now; n++;
            { int nz = 0; for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) if (s_mix_buffer[i] != 0.0f) { nz = 1; break; }
              if (!nz) z++; }
            if (now - t0 >= 5000) {
                fprintf(stderr, "[audio-rate] %.1f blocks/s (real time 187.5), silent %.1f%%, dropped %.0f frames/s, underruns %llu, device buf %u, holes %u+%u%c",
                        n * 1000.0 / (now - t0), n ? 100.0 * z / n : 0.0, (g_audio_dropped - d0) * 1000.0 / (now - t0),
                        g_audio_underruns - u0,
                #if AUDIO_BACKEND_WASAPI
                        (unsigned)s_wasapi_buf_frames, g_audio_holes[0], g_audio_holes[1], 10);
#else
                        0u, g_audio_holes[0], g_audio_holes[1], 10);
#endif
                g_audio_holes[0] = g_audio_holes[1] = 0;
                n = z = 0; d0 = g_audio_dropped; u0 = g_audio_underruns; t0 = now; } } }

        /* Notify event queues */
        audio_notify_event_queues();
    }

    printf("[cellAudio] Mixing thread stopped\n");
    return 0;
}
#else
/* Pacing of the POSIX mixer: one block every 5.33 ms on the monotonic clock,
 * the way the console's audio hardware consumes them, nudged to follow the
 * host device's own clock.
 *
 * A guest mixer (libmixer's cellSurMixer, which Resistance plays its movies
 * through) writes the block after the one being played when the period event
 * wakes it, so it needs those 5.33 ms between two reads. The previous loop
 * mixed whenever the device queue fell under four blocks: each time the
 * device took its buffer, the loop read two blocks back to back, the second
 * before the guest had been woken to fill it, and every other block of a
 * movie's soundtrack came out empty -- a 94 Hz buzz over half the audio.
 *
 * So blocks are due at t0 + n periods. With a device, the queue is kept
 * between AUDIO_PACE_LOW and AUDIO_PACE_HIGH blocks by moving t0 a quarter
 * period at most once a block (never two blocks closer than 0.75 periods),
 * judged on the queue right after the previous block went out; with none
 * (queued < 0) the clock alone paces. More than 100 ms behind (a debugger
 * stop, a suspended machine) drops the backlog instead of mixing it in a
 * burst. Returns 0 when the next block is due, else the nanoseconds to wait. */
#define AUDIO_PACE_LOW   3u
#define AUDIO_PACE_HIGH  10u
typedef struct {
    unsigned long long t0_ns;   /* when block 0 was due */
    unsigned long long mixed;   /* blocks mixed so far  */
    int                decided; /* the next block's nudge has been chosen */
} AudioPace;

static unsigned long long audio_pace_due(const AudioPace* p)
{
    /* 256 samples at 48 kHz = 16e6/3 ns, kept exact over a long run. */
    return p->t0_ns + p->mixed * 16000000ull / 3ull;
}

static unsigned long long audio_pace_wait(AudioPace* p, unsigned long long now_ns, long queued)
{
    const unsigned long long period = 16000000ull / 3ull;
    /* Once a block, on the first call after the previous one was mixed: a
     * nudge chosen later, when the block is already due, would land on top
     * of the next block's and bring two reads half a period apart. */
    if (!p->decided) {
        p->decided = 1;
        if (queued >= 0 && p->mixed > 0) {
            if ((unsigned long)queued < AUDIO_PACE_LOW * CELL_AUDIO_BLOCK_SAMPLES) p->t0_ns -= period / 4;
            else if ((unsigned long)queued > AUDIO_PACE_HIGH * CELL_AUDIO_BLOCK_SAMPLES) p->t0_ns += period / 4;
        }
    }
    const unsigned long long due = audio_pace_due(p);
    if (now_ns < due) return due - now_ns;
    if (now_ns - due > 100000000ull)
        p->t0_ns = now_ns - p->mixed * 16000000ull / 3ull;
    p->mixed++;
    p->decided = 0;
    return 0;
}

static unsigned long long audio_now_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ull + (unsigned long long)t.tv_nsec;
}

/* Frames queued at the device, or -1 with no device open. */
static long audio_backend_queue(void)
{
#if AUDIO_BACKEND_SDL2
    if (!s_sdl_audio_dev) return -1;
#endif
    return (long)audio_backend_queued_samples();
}

static void* audio_mix_thread_func(void* arg)
{
    (void)arg;
    printf("[cellAudio] Mixing thread started\n");

    unsigned long blocks = 0; float peak = 0.0f;
    AudioPace pace = { audio_now_ns(), 0, 0 };   /* t0, mixed, decided */
    while (s_mix_thread_running) {
        const unsigned long long wait = audio_pace_wait(&pace, audio_now_ns(), audio_backend_queue());
        if (wait) {
            /* Sleep most of the way, then poll: usleep overshoots by up to a
             * few hundred microseconds. */
            if (wait > 1500000ull) usleep((useconds_t)((wait - 1000000ull) / 1000ull));
            else if (wait > 200000ull) usleep(100);
            continue;
        }
        /* A producer that is a little late (the guest thread woken by the
         * last period event has not been scheduled yet: a loaded host) gets
         * up to 4 ms before its block is played as silence -- RPCS3 waits for
         * "touched" buffers the same way. The clock does not move: the next
         * block keeps its due time, and the device queue (3+ blocks) covers
         * the delay. Under load (12 busy cores) this took the isolated empty
         * blocks of a 45 s movie stretch from 49-82 to 0. */
        { const unsigned long long g0 = audio_now_ns();
          while (s_mix_thread_running && audio_port_starved() &&
                 audio_now_ns() - g0 < 4000000ull)
              usleep(100); }
        audio_mix_one_block();
        audio_submit_mix();
        audio_notify_event_queues();
        /* AUDIO_PEAK=1: the loudest sample of every second of mixed output (187.5 blocks of 256). */
        { static int pk_on = -1; if (pk_on < 0) pk_on = getenv("AUDIO_PEAK") ? 1 : 0;
          if (pk_on) {
              const float pk = audio_mix_peak(); if (pk > peak) peak = pk;
              if (++blocks % 188 == 0) { fprintf(stderr, "[audio-peak] %lu s: peak %.4f\n", blocks / 188, peak); peak = 0.0f; }
          } }
    }

    printf("[cellAudio] Mixing thread stopped\n");
    return NULL;
}
#endif

static int audio_start_mix_thread(void)
{
    s_mix_thread_running = 1;

#ifdef _WIN32
    s_mix_thread = (HANDLE)_beginthreadex(NULL, 0, audio_mix_thread_func,
                                           NULL, 0, NULL);
    if (!s_mix_thread) {
        s_mix_thread_running = 0;
        return -1;
    }
#else
    if (pthread_create(&s_mix_thread, NULL, audio_mix_thread_func, NULL) != 0) {
        s_mix_thread_running = 0;
        return -1;
    }
    s_mix_thread_live = 1;
#endif
    return 0;
}

static void audio_stop_mix_thread(void)
{
    s_mix_thread_running = 0;
#ifdef _WIN32
    if (s_mix_thread) {
        WaitForSingleObject(s_mix_thread, 2000);
        CloseHandle(s_mix_thread);
        s_mix_thread = NULL;
    }
#else
    /* Same guard. Init leaves s_audio_initialized set even when the mixing
     * thread failed to start ("continuing silently" -- a title without audio
     * still runs), so Quit reaches here with s_mix_thread never assigned.
     * pthread_t has no null value to test, and joining an unset one, or
     * joining the same one twice, is undefined rather than an error the way
     * WaitForSingleObject on a null HANDLE is. */
    if (s_mix_thread_live) {
        pthread_join(s_mix_thread, NULL);
        s_mix_thread_live = 0;
    }
#endif
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellAudioInit(void)
{
    /* PS3_NO_AUDIO=1: report that the audio library is unavailable. Middleware
     * that drives its mixer through SPU jobs will otherwise block the whole boot
     * waiting on work we cannot yet complete; failing here lets a title take its
     * silent path and keep going. Diagnostic first, workaround second. */
    if (getenv("PS3_NO_AUDIO")) {
        printf("[cellAudio] PS3_NO_AUDIO -- reporting audio unavailable\n");
        return (s32)CELL_AUDIO_ERROR_NOT_INIT;
    }
    printf("[cellAudio] Init()\n");

    if (s_audio_initialized)
        return CELL_AUDIO_ERROR_ALREADY_INIT;

    memset(s_ports, 0, sizeof(s_ports));
    memset(s_notify_queues, 0, sizeof(s_notify_queues));
    mutex_init(&s_audio_mutex);

    if (audio_backend_init() < 0) {
        printf("[cellAudio] WARNING: Audio backend init failed, continuing silently\n");
        /* Don't fail -- games should still run without audio */
    }


#ifdef _WIN32
    { LARGE_INTEGER f, c;
      QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
      s_audio_start_us = (u64)(c.QuadPart / f.QuadPart) * 1000000ULL +
          (u64)(c.QuadPart % f.QuadPart) * 1000000ULL / (u64)f.QuadPart; }
#else
    { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
      s_audio_start_us = (u64)ts.tv_sec * 1000000ULL + (u64)ts.tv_nsec / 1000ULL; }
#endif
    audio_dump_open();
    if (audio_start_mix_thread() < 0) {
        printf("[cellAudio] WARNING: Could not start mixing thread\n");
    }

    s_audio_initialized = 1;
    return CELL_OK;
}

s32 cellAudioQuit(void)
{
    printf("[cellAudio] Quit()\n");

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    audio_stop_mix_thread();
    audio_backend_shutdown();
    audio_dump_close();

    /* Port buffers live in the guest vm_base arena (bump-allocated, not host
     * malloc) -- just drop the references; never free() them. */
    for (int i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        s_ports[i].buffer = NULL;
        s_ports[i].in_use = 0;
        s_ports[i].running = 0;
    }

    mutex_destroy(&s_audio_mutex);
    s_audio_initialized = 0;
    return CELL_OK;
}

s32 cellAudioPortOpen(const CellAudioPortParam* param, u32* portNum)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    /* param / portNum are GUEST addresses; translate, and read the BE u64 param
     * fields via vm_read64 (a raw param->nChannel faults / is host-endian). */
    uint32_t param_ea   = (uint32_t)(uintptr_t)param;
    uint32_t portNum_ea = (uint32_t)(uintptr_t)portNum;
    if (!param_ea || !portNum_ea)
        return CELL_AUDIO_ERROR_PARAM;

    u64 nch  = vm_read64(param_ea + 0);   /* nChannel */
    u64 nblk = vm_read64(param_ea + 8);   /* nBlock   */
    u64 attr = vm_read64(param_ea + 16);  /* attr     */
    /* level is honoured only with CELL_AUDIO_PORTATTR_INITLEVEL, as on the
     * console (RPCS3 cellAudioPortOpen); otherwise the port starts at 1.0. */
    float level = 1.0f;
    if (attr & 0x1000ull) {
        u32 lv = vm_read32(param_ea + 24);
        memcpy(&level, &lv, 4);
    }
    printf("[cellAudio] PortOpen(nChannel=%llu, nBlock=%llu, attr=0x%llX, level=%.3f) at t=%.2fs\n",
           (unsigned long long)nch, (unsigned long long)nblk,
           (unsigned long long)attr, level, audio_clock_s());

    if (nch != CELL_AUDIO_PORT_2CH && nch != CELL_AUDIO_PORT_8CH)
        return CELL_AUDIO_ERROR_PARAM;
    if (nblk != CELL_AUDIO_BLOCK_8 && nblk != CELL_AUDIO_BLOCK_16 && nblk != CELL_AUDIO_BLOCK_32)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);

    /* Find a free port slot */
    s32 found = -1;
    for (u32 i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        if (!s_ports[i].in_use) {
            found = (s32)i;
            break;
        }
    }

    if (found < 0) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_FULL;
    }

    AudioPortSlot* port = &s_ports[found];
    port->in_use  = 1;
    port->running = 0;
    /* Store the decoded (host-endian) param values, not the raw BE guest bytes. */
    memset(&port->param, 0, sizeof(port->param));
    port->was_playing  = 0;
    port->ever_started = 0;
    port->param.nChannel = nch;
    port->param.nBlock   = nblk;
    port->param.attr     = attr;
    port->param.level    = level;
    port->read_index  = 0;
    port->write_index = 0;

    /* Allocate the audio buffer in GUEST memory so portAddr/readIndexAddr are
     * real guest addresses (the game's mixer -- FMOD here -- writes PCM there
     * and validates them as 1 MB-aligned guest pointers; a host pointer trips
     * its "invalid parameter" check). Bump from a free main-memory window; the
     * host-side `buffer` is just the vm_base view of the same bytes. */
    u32 buf_samples = (u32)(nblk * CELL_AUDIO_BLOCK_SAMPLES * nch);
    port->buf_size = buf_samples * (u32)sizeof(float);

    /* Each port reuses its own two pages in the runtime-owned audio window. */
    _Static_assert(CELL_AUDIO_PORT_MAX * VM_AUDIO_PORT_SIZE <= VM_AUDIO_SIZE,
                   "Audio port storage exceeds its reserved VM window");
    u32 guest_buf = VM_AUDIO_BASE + (u32)found * VM_AUDIO_PORT_SIZE;
    u32 guest_ridx = guest_buf + 0x100000u;
    if (vm_commit(guest_buf, VM_AUDIO_PORT_SIZE) != CELL_OK) {
        port->in_use = 0;
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_AUDIOSYSTEM;
    }

    port->buffer = (float*)(vm_base + guest_buf);     /* host view of guest buffer */
    memset(port->buffer, 0, port->buf_size);
    vm_write64(guest_ridx, 0);                        /* read index counter (u64, see below) */

    port->port_addr     = guest_buf;
    port->read_idx_addr = guest_ridx;

    vm_write32(portNum_ea, (u32)found);   /* guest out-param */

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortClose(u32 portNum)
{
    printf("[cellAudio] PortClose(port=%u) at t=%.2fs\n", portNum, audio_clock_s());

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    mutex_lock(&s_audio_mutex);

    if (!s_ports[portNum].in_use) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    }

    /* buffer points INTO the guest vm_base arena (reserved per port in PortOpen),
     * not a host malloc -- do NOT free() it (that corrupts the host heap).
     * The guest window is reclaimed wholesale when vm_base is released. */
    s_ports[portNum].buffer = NULL;

    s_ports[portNum].in_use  = 0;
    s_ports[portNum].running = 0;

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortStart(u32 portNum)
{
    printf("[cellAudio] PortStart(port=%u) at t=%.2fs\n", portNum, audio_clock_s());

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_ALREADY_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 1;
    s_ports[portNum].ever_started = 1;
    mutex_unlock(&s_audio_mutex);

    return CELL_OK;
}

s32 cellAudioPortStop(u32 portNum)
{
    printf("[cellAudio] PortStop(port=%u) at t=%.2fs\n", portNum, audio_clock_s());

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_NOT_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 0;
    mutex_unlock(&s_audio_mutex);

    return CELL_OK;
}

s32 cellAudioGetPortBlockTag(u32 portNum, u64 blockNo, u64* tag)
{
    uint32_t tag_ea = (uint32_t)(uintptr_t)tag;
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PARAM;
    if (!s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (!tag_ea)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];
    u64 nblk = port->param.nBlock ? port->param.nBlock : 1;
    if (blockNo >= nblk) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PARAM;
    }
    u64 t = port->read_index + blockNo - (port->read_index % nblk);
    if (blockNo < port->read_index % nblk) t += nblk;
    mutex_unlock(&s_audio_mutex);

    vm_write64(tag_ea, t);
    return CELL_OK;
}

s32 cellAudioGetPortTimestamp(u32 portNum, u64 tag, u64* stamp)
{
    uint32_t stamp_ea = (uint32_t)(uintptr_t)stamp;
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PARAM;
    if (!s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (!stamp_ea)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    u64 global_counter = s_ports[portNum].read_index;
    mutex_unlock(&s_audio_mutex);
    if (tag > global_counter)
        return CELL_AUDIO_ERROR_TAG_NOT_FOUND;

    u64 t = s_audio_start_us + tag * 256000000ULL / 48000ULL;
    vm_write64(stamp_ea, t);
    return CELL_OK;
}

/* cellAudioCreateNotifyEventQueue(sys_event_queue_t* id, u64* key)
 *
 * Creates the queue the game will block on for audio-period events, and
 * registers it for notification in one step -- the game never calls
 * SetNotifyEventQueue for this one.
 *
 * This was missing entirely. YDKJ imports it, got the unimplemented-NID path,
 * and then blocked on an id nothing had created: sys_event_queue_receive
 * answered CELL_ESRCH every time and the title logged 1.7 MILLION
 * "ERROR - LIBAUDIO DROPOUT" lines in a 60 s run.
 *
 * Both params are GUEST addresses and both are big-endian out-values; the id
 * is 32-bit and the key 64-bit, so they cannot be written with a plain store. */
s32 cellAudioCreateNotifyEventQueue(u32* id, u64* key)
{
    uint32_t id_ea  = (uint32_t)(uintptr_t)id;
    uint32_t key_ea = (uint32_t)(uintptr_t)key;

    printf("[cellAudio] CreateNotifyEventQueue(id_ea=0x%08X key_ea=0x%08X)\n",
           id_ea, key_ea);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (!id_ea || !key_ea)
        return CELL_AUDIO_ERROR_PARAM;

    /* A distinct key per queue: the notify path resolves queues BY KEY, and the
     * game creates its other queues with key 0, so reusing 0 here would make
     * find_queue_by_key ambiguous and post audio events to whichever matched. */
    static uint64_t s_next_key = 0x8000000000000001ull;
    uint64_t k = s_next_key++;

    uint32_t qid = sys_event_queue_create_direct(k, 8);
    if (!qid)
        return CELL_AUDIO_ERROR_EVENT_QUEUE;

    mutex_lock(&s_audio_mutex);
    int slot = -1;
    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) { slot = i; break; }
    }
    if (slot < 0) { mutex_unlock(&s_audio_mutex); return CELL_AUDIO_ERROR_PARAM; }
    s_notify_queues[slot].in_use = 1;
    s_notify_queues[slot].key    = k;
    mutex_unlock(&s_audio_mutex);

    vm_write32(id_ea, qid);
    vm_write64(key_ea, k);
    printf("[cellAudio] CreateNotifyEventQueue -> id=%u key=0x%llX\n",
           qid, (unsigned long long)k);
    return CELL_OK;
}

s32 cellAudioSetNotifyEventQueue(u64 key)
{
    printf("[cellAudio] SetNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) {
            s_notify_queues[i].in_use = 1;
            s_notify_queues[i].key = key;
            mutex_unlock(&s_audio_mutex);
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    return CELL_AUDIO_ERROR_PARAM; /* no free slots */
}

s32 cellAudioRemoveNotifyEventQueue(u64 key)
{
    printf("[cellAudio] RemoveNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (s_notify_queues[i].in_use && s_notify_queues[i].key == key) {
            s_notify_queues[i].in_use = 0;
            mutex_unlock(&s_audio_mutex);
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    return CELL_AUDIO_ERROR_PARAM;
}

s32 cellAudioGetPortConfig(u32 portNum, CellAudioPortConfig* config)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!config)
        return CELL_AUDIO_ERROR_PARAM;

    /* `config` is a GUEST address; write the BE struct field-by-field at its
     * guest offsets (8-byte aligned u64s). */
    uint32_t cfg = (uint32_t)(uintptr_t)config;
    if (!cfg)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];

    /* CellAudioPortConfig (cell/audio.h). sys_addr_t is uintptr_t, which on the
     * 32-bit PPU ABI is FOUR bytes -- not eight. Writing the two sys_addr_t
     * fields as u64 shifted every field past it and left the game reading the
     * high (zero) half of readIndexAddr as its pointer:
     *   sys_addr_t readIndexAddr;  u32 @  0
     *   uint32_t   status;         u32 @  4
     *   uint64_t   nChannel;       u64 @  8
     *   uint64_t   nBlock;         u64 @ 16
     *   uint32_t   portSize;       u32 @ 24
     *   sys_addr_t portAddr;       u32 @ 28      -> 32 bytes */
    vm_write32(cfg +  0, (u32)port->read_idx_addr);                          /* readIndexAddr */
    vm_write32(cfg +  4, port->running ? CELL_AUDIO_STATUS_RUN
                                       : CELL_AUDIO_STATUS_READY);           /* status */
    vm_write64(cfg +  8, port->param.nChannel);                             /* nChannel */
    vm_write64(cfg + 16, port->param.nBlock);                               /* nBlock */
    vm_write32(cfg + 24, port->buf_size);                                   /* portSize */
    vm_write32(cfg + 28, (u32)port->port_addr);                             /* portAddr */

    { static int _n = 0; if (PS3_LOG_FIRST(_n, 24))
        fprintf(stderr, "[cellAudio] GetPortConfig(port=%u) status=%s bufEA=0x%08X ridxEA=0x%08X\n",
                portNum, port->running ? "RUN" : "READY",
                (u32)port->port_addr, (u32)port->read_idx_addr); }

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortGetStatus(u32 portNum, u32* status)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !status)
        return CELL_AUDIO_ERROR_PARAM;

    if (!s_ports[portNum].in_use) {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_CLOSE);
    } else if (s_ports[portNum].running) {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_RUN);
    } else {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_READY);
    }

    return CELL_OK;
}

s32 cellAudioSetPersonalDevice(s32 iPersonalStream, s32 iDevice)
{
    (void)iPersonalStream;
    (void)iDevice;
    printf("[cellAudio] SetPersonalDevice(stream=%d, device=%d) - stub\n",
           iPersonalStream, iDevice);
    return CELL_OK;
}

s32 cellAudioUnsetPersonalDevice(s32 iPersonalStream)
{
    (void)iPersonalStream;
    printf("[cellAudio] UnsetPersonalDevice(stream=%d) - stub\n", iPersonalStream);
    return CELL_OK;
}
