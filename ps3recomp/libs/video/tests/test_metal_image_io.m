/* The Metal backend's image files for texture packs (eng_image_load /
 * eng_image_save, PS3_MODS and PS3_MODS_DUMP): what the dump writes reads
 * back byte for byte, straight alpha included; a grey PNG and a file that is
 * not an image.
 *
 *   clang -fobjc-arc -I include -I libs/video libs/video/tests/test_metal_image_io.m \
 *     -Wl,-dead_strip -framework Metal -framework Cocoa -framework QuartzCore \
 *     -o /tmp/test_metal_image_io && /tmp/test_metal_image_io
 */
#include "../rsx_metal_backend.m"
#include <assert.h>

float cellAvconfExt_display_gamma(void) { return 1.0f; }

int main(void)
{
    @autoreleasepool {
        char dir[] = "/tmp/metal_image_io_XXXXXX";
        assert(mkdtemp(dir));
        char path[256];
        snprintf(path, sizeof path, "%s/sub.png", dir);

        /* 3x2 with a padded source row, translucent texels and alpha 0 */
        enum { W = 3, H = 2, ROW = 16 };
        u8 src[ROW * H];
        memset(src, 0xEE, sizeof src);
        for (u32 y = 0; y < H; y++)
            for (u32 x = 0; x < W; x++) {
                u8* p = src + y * ROW + x * 4;
                p[0] = (u8)(40 * x + 7); p[1] = (u8)(90 * y + 3); p[2] = 200; p[3] = (u8)(x == 0 ? 0 : x == 1 ? 0x80 : 0xFF);
            }
        assert(eng_image_save(NULL, path, src, W, H, ROW));
        for (int i = 0; i < 200 && access(path, F_OK) != 0; i++) usleep(10 * 1000);   /* written later */
        usleep(50 * 1000);
        u8* back = NULL; u32 w = 0, h = 0;
        assert(eng_image_load(NULL, path, &back, &w, &h) && back && w == W && h == H);
        for (u32 y = 0; y < H; y++)
            assert(!memcmp(back + y * W * 4, src + y * ROW, W * 4));   /* exact, alpha 0 too */
        free(back);

        /* a grey PNG reads as grey RGBA */
        NSBitmapImageRep* g = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:2 pixelsHigh:1
            bitsPerSample:8 samplesPerPixel:1 hasAlpha:NO isPlanar:NO colorSpaceName:NSDeviceWhiteColorSpace
            bytesPerRow:2 bitsPerPixel:8];
        g.bitmapData[0] = 17; g.bitmapData[1] = 250;
        snprintf(path, sizeof path, "%s/grey.png", dir);
        assert([[g representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(path) atomically:YES]);
        assert(eng_image_load(NULL, path, &back, &w, &h) && w == 2 && h == 1);
        assert(back[0] == 17 && back[1] == 17 && back[2] == 17 && back[3] == 255 && back[4] == 250);
        free(back);

        /* not an image */
        snprintf(path, sizeof path, "%s/notes.png", dir);
        FILE* f = fopen(path, "w"); fputs("hello", f); fclose(f);
        assert(!eng_image_load(NULL, path, &back, &w, &h) && back == NULL);
        snprintf(path, sizeof path, "rm -rf '%s'", dir);
        if (system(path) != 0) fprintf(stderr, "could not remove %s\n", dir);
        puts("test_metal_image_io: ok");
    }
    return 0;
}
