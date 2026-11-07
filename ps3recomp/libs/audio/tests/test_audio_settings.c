/* PS3_AUDIO_VOLUME's gain curve and PS3_AUDIO_DEVICE's name matching.
 * macOS: clang -std=gnu17 -I include -I /opt/homebrew/include
 *   libs/audio/tests/test_audio_settings.c -L /opt/homebrew/lib -lSDL2
 *   -Wl,-dead_strip -o /tmp/test_audio_settings && /tmp/test_audio_settings
 */
#include "../cellAudio.c"
#include <assert.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

int main(void)
{
    assert(audio_volume_gain(NULL) == 1.0f);
    assert(audio_volume_gain("") == 1.0f);
    assert(audio_volume_gain("loud") == 1.0f);
    assert(audio_volume_gain("100") == 1.0f);
    assert(audio_volume_gain("150") == 1.0f);
    assert(audio_volume_gain("0") == 0.0f);
    assert(audio_volume_gain("-5") == 0.0f);
    assert(fabsf(audio_volume_gain("50") - 0.25f) < 1e-6f);
    assert(fabsf(audio_volume_gain("25") - 0.0625f) < 1e-6f);

    const char* names[] = { "MacBook Pro Speakers", "Erik's AirPods Pro", "External Headphones", "DELL U2720Q" };
    assert(audio_match_device(NULL, names, 4) == -1);
    assert(audio_match_device("", names, 4) == -1);
    assert(audio_match_device("External Headphones", names, 4) == 2);
    assert(audio_match_device("dell u2720q", names, 4) == 3);
    assert(audio_match_device("airpods", names, 4) == 1);
    assert(audio_match_device("Speakers", names, 4) == 0);
    assert(audio_match_device("HDMI", names, 4) == -1);
    const char* dup[] = { "Headphones 2", "Headphones" };
    assert(audio_match_device("headphones", dup, 2) == 1);   /* a whole name beats a part */
    /* The in-game menu's volume (ps3_overlay.mm): PS3_AUDIO_VOLUME first,
     * then the menu's value, live. */
    setenv("PS3_AUDIO_VOLUME", "50", 1);
    assert(cellAudio_host_volume() == 50 && fabsf(cellAudio_host_gain() - 0.25f) < 1e-6f);
    cellAudio_host_set_volume(25);
    assert(cellAudio_host_volume() == 25 && fabsf(cellAudio_host_gain() - 0.0625f) < 1e-6f);
    cellAudio_host_set_volume(140);
    assert(cellAudio_host_volume() == 100 && cellAudio_host_gain() == 1.0f);
    printf("test_audio_settings: ok\n");
    return 0;
}
