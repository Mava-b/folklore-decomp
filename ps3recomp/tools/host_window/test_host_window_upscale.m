/* The window host's MetalFX path (PS3_UPSCALE=metalfx, host_window.m) with
 * no window: a surface-sized frame through upscale() to a larger size, the
 * fallback when the source cannot be read, and the drawable staying at the
 * surfaces' size without it.
 *
 *   clang -fobjc-arc -I libs/video tools/host_window/test_host_window_upscale.m \
 *         -framework AppKit -framework Metal -framework QuartzCore -framework IOSurface \
 *         -o /tmp/test_host_window_upscale && /tmp/test_host_window_upscale
 */
#define main host_window_main
#include "host_window.m"
#undef main
#include <assert.h>

static id<MTLTexture> frame(NSUInteger w, NSUInteger h, MTLTextureUsage usage)
{
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:w height:h mipmapped:NO];
    td.usage = usage;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [g.dev newTextureWithDescriptor:td];
    uint8_t* px = malloc(w * h * 4);
    for (NSUInteger y = 0; y < h; y++)
        for (NSUInteger x = 0; x < w; x++) {
            uint8_t* p = px + (y * w + x) * 4;
            p[0] = (uint8_t)(x * 255 / (w - 1)); p[1] = (uint8_t)(y * 255 / (h - 1)); p[2] = 128; p[3] = 255;
        }
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:px bytesPerRow:w * 4];
    free(px);
    return t;
}

int main(void)
{
    @autoreleasepool {
        g.dev = MTLCreateSystemDefaultDevice();
        if (!g.dev) { printf("test_host_window_upscale: no Metal device, skipped\n"); return 0; }
        g.queue = [g.dev newCommandQueue];
        g.layer = [CAMetalLayer layer];
        g.width = 64; g.height = 36;

        /* without PS3_UPSCALE the drawable is the surfaces' size */
        g.upscale_on = 0;
        keep_drawable();
        assert(g.layer.drawableSize.width == 64 && g.layer.drawableSize.height == 36);

        g.upscale_on = 1;
        id<MTLTexture> src = frame(64, 36, MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
        id<MTLCommandBuffer> cb = [g.queue commandBuffer];
        id<MTLTexture> out = upscale(cb, src, 128, 72);
        if (!out) {
            /* MetalFX missing on this machine: the path turns itself off */
            assert(!g.upscale_on);
            printf("test_host_window_upscale: MetalFX not available here, fallback checked\n");
            return 0;
        }
        assert(out.width == 128 && out.height == 72 && out.storageMode == MTLStorageModePrivate);
        /* read it back through a shared copy, as the window's blit would */
        id<MTLTexture> back = frame(128, 72, MTLTextureUsageShaderRead);
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:out toTexture:back];
        [b endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        assert(cb.status == MTLCommandBufferStatusCompleted);
        uint8_t* px = malloc(128 * 72 * 4);
        [back getBytes:px bytesPerRow:128 * 4 fromRegion:MTLRegionMake2D(0, 0, 128, 72) mipmapLevel:0];
        /* the gradient survives: dark at the left, bright at the right, red kept */
        const uint8_t* l = px + (36 * 128 + 2) * 4;
        const uint8_t* r = px + (36 * 128 + 125) * 4;
        assert(l[0] < 40 && r[0] > 215);
        assert(abs((int)l[2] - 128) < 24 && abs((int)r[2] - 128) < 24);
        free(px);
        /* the same sizes reuse the scaler */
        id scaler = g.fx_scaler;
        cb = [g.queue commandBuffer];
        assert(upscale(cb, src, 128, 72) == out && g.fx_scaler == scaler);
        [cb commit]; [cb waitUntilCompleted];

        /* a source the scaler may not read: it gives up, presents plain */
        id<MTLTexture> bad = frame(64, 36, MTLTextureUsageRenderTarget);
        cb = [g.queue commandBuffer];
        assert(upscale(cb, bad, 128, 72) == nil);
        assert(!g.upscale_on && g.fx_scaler == nil);
        assert(g.layer.drawableSize.width == 64 && g.layer.drawableSize.height == 36);
        printf("test_host_window_upscale: ok\n");
    }
    return 0;
}
