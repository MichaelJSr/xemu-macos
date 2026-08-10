//
// xemu User Interface — Metal presentation helpers (implementation)
//
// Objective-C++ twin of gl-helpers.cc for the Metal presentation
// backend. Pixel data loading and coordinate conventions deliberately
// replicate the GL path (textures loaded vertically flipped, offscreen
// target row order matching GL FBOs) so all existing call-site math
// and ImGui UV flips work unchanged.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#ifdef __APPLE__

#import <Metal/Metal.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "metal-helpers.hh"
#include "stb_image.h"

#include "imgui.h"
#include "imgui_impl_metal.h"

extern "C" {
#include "ui/xemu-metal.h"
}

// ---------------------------------------------------------------------------
// Shader source
// ---------------------------------------------------------------------------

static NSString *const kShaderSource = @R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VertUniforms {
    float4 scale_offset;
    float4 tex_scale_offset;
    uint flip_y;
    uint flip_ndc;
    uint pad0;
    uint pad1;
};

struct FragUniforms {
    float4 color_primary;
    float4 color_secondary;
    float4 color_fill;
    float time;
    float scale;
    float pad0;
    float pad1;
};

struct VSOut {
    float4 position [[position]];
    float2 texcoord;
    float2 screen01;
};

// Fullscreen-decal quad as a triangle strip: BL, BR, TL, TR with
// GL-style coordinates (+y up, texcoord v=0 at the bottom). Matches
// the vertex layout + shader of gl-helpers.cc NewDecalShader.
vertex VSOut decal_vs(uint vid [[vertex_id]],
                      constant VertUniforms &u [[buffer(0)]])
{
    const float2 pos[4] = {
        float2(-1.0, -1.0), float2(1.0, -1.0),
        float2(-1.0,  1.0), float2(1.0,  1.0),
    };
    const float2 tc[4] = {
        float2(0.0, 0.0), float2(1.0, 0.0),
        float2(0.0, 1.0), float2(1.0, 1.0),
    };

    float2 t = tc[vid];
    if (u.flip_y != 0) {
        t.y = 1.0 - t.y;
    }

    VSOut o;
    o.texcoord = t * u.tex_scale_offset.xy + u.tex_scale_offset.zw;
    o.screen01 = (pos[vid] + 1.0) * 0.5;
    float2 p = pos[vid] * u.scale_offset.xy + u.scale_offset.zw;
    if (u.flip_ndc != 0) {
        // Offscreen targets replicate GL FBO row order (row 0 = scene
        // bottom) so ImGui call sites with flipped UVs work unchanged.
        p.y = -p.y;
    }
    o.position = float4(p, 0.0, 1.0);
    return o;
}

// 2-color mask decal (port of mask_frag_src)
fragment float4 mask_fs(VSOut in [[stage_in]],
                        constant FragUniforms &u [[buffer(0)]],
                        texture2d<float> tex [[texture(0)]],
                        sampler smp [[sampler(0)]])
{
    float4 t = tex.sample(smp, in.texcoord);
    float4 o = u.color_fill;
    o.rgb += mix(u.color_secondary.rgb, u.color_primary.rgb, t.r);
    o.a += t.a - t.b;
    return o;
}

// Gamma blit (port of image_gamma_frag_src): 256-entry DAC palette,
// one packed uint per entry (r at bits 0-7, g 8-15, b 16-23).
fragment float4 gamma_fs(VSOut in [[stage_in]],
                         constant uint *palette [[buffer(0)]],
                         texture2d<float> tex [[texture(0)]],
                         sampler smp [[sampler(0)]])
{
    float4 col = tex.sample(smp, in.texcoord);
    uint r = (palette[uint(clamp(col.r, 0.0, 1.0) * 255.0)] >> 0) & 0xffu;
    uint g = (palette[uint(clamp(col.g, 0.0, 1.0) * 255.0)] >> 8) & 0xffu;
    uint b = (palette[uint(clamp(col.b, 0.0, 1.0) * 255.0)] >> 16) & 0xffu;
    return float4(float(r) / 255.0, float(g) / 255.0, float(b) / 255.0,
                  col.a);
}

// Animated SDF logo (port of ui/shader/xemu-logo.frag)
constant float pxRange         = 6.0;
constant float4 textPos        = float4(0.01, 0.0, 0.98, 0.125);
constant float logo_pi         = 3.14159265359;
constant float lineWidth       = 0.175;
constant float duration        = 1.25;
constant float pause           = 6.0;
constant int numParticles      = 35;
constant int numSpotlights     = 5;

static float logo_random(float co)
{
    return fract(abs(sin(co * 12.989)) * 43758.545);
}

static float logo_median(float r, float g, float b)
{
    return max(min(r, g), min(max(r, g), b));
}

static float quaImpulse(float k, float x)
{
    return 2.0 * sqrt(k) * x / (1.0 + k * x * x);
}

static float getCurrentTime(float iTime)
{
    return fmod(iTime, duration + pause) / duration;
}

static float getBox(float2 uv, float x, float width)
{
    float lhs = sign(clamp(x - uv.x, 0.0, 1.0));
    float rhs = sign(clamp(x - uv.x + width, 0.0, 1.0));
    return rhs - lhs;
}

static float getSweepingLinePos(float iTime)
{
    return getCurrentTime(iTime) - lineWidth + textPos.x;
}

static float getSweepingLine(float2 uv, float iTime)
{
    return getBox(uv, getSweepingLinePos(iTime), lineWidth);
}

static float getGradients(float2 uv, float iTime)
{
    float t = getCurrentTime(iTime);
    float l_s = abs(cos(t * logo_pi * 2.0));
    float pos = t - uv.x + textPos.x;
    float left = l_s * smoothstep(0.0, 1.0,
        0.5 - abs(pos - lineWidth) * (20.0 + 80.0 * (1.0 - l_s)));
    float r_s = abs(sin(t * logo_pi * 2.0));
    float right = r_s * smoothstep(0.0, 1.0,
        0.5 - abs(pos) * (20.0 + 80.0 * (1.0 - r_s)));
    float gradient_y = smoothstep(0.55, 1.0, 1.0 - abs(0.5 - uv.y));
    return (left + right) * gradient_y;
}

static float2 getSpotlightPos(int i, float iTime)
{
    float t = getCurrentTime(iTime);
    float2 initialPos = textPos.zw * float2(
        float(i) / float(numSpotlights - 1),
        sign(logo_random(float(i + 62)) - 0.6) * 2.0);

    float2 velocity;
    velocity.x = sign(logo_random(float(i + 63)) - 0.5) * 0.7 *
                 (0.3 + 0.6 * logo_random(float(i + 100)));
    velocity.y = -sign(initialPos.y) * 0.8 *
                 (0.1 + 0.9 * logo_random(float(i + 62)));
    return initialPos + velocity * t + float2(textPos.x, 0.5);
}

static float getSpotlights(float2 uv, float iTime)
{
    float t = getCurrentTime(iTime);
    float right = smoothstep(0.3, 0.7,
        0.8 - 8.0 * abs(t - uv.x + textPos.x + 0.05));

    float c = 0.0;
    for (int j = 0; j < numSpotlights; j++) {
        float2 pos = getSpotlightPos(j, iTime);
        float d = distance(uv, pos);
        c += (1.0 - smoothstep(0.04, 0.07835, d));
    }

    return 0.6 * right + 0.4 * c;
}

static float2 getParticleInitialPosition(int i)
{
    return textPos.zw * float2(
        float(i) / float(numParticles - 1),
        sign(logo_random(float(i)) - 0.2));
}

static float logo_prob(float p, int i)
{
    return sign(clamp(logo_random(float(i * 30)) - (1.0 - p), 0.0, 1.0));
}

static float getParticleLifespan(int i)
{
    return 1.0 + 1.25 * exp(-10.0 * logo_random(float(i * 30))) +
           0.5 * logo_prob(0.3, i);
}

static float getParticleTime(int i, float iTime)
{
    return getCurrentTime(iTime) - getParticleInitialPosition(i).x;
}

static float getParticleAlive(int i, float iTime)
{
    return clamp(sign(getParticleTime(i, iTime)), 0.0, 1.0);
}

static float getParticleIntensity(int i, float iTime)
{
    return getParticleAlive(i, iTime) *
           clamp(getParticleLifespan(i) - getParticleTime(i, iTime),
                 0.0, 1.0);
}

static float2 getParticlePosition(int i, float iTime)
{
    float pt = getParticleTime(i, iTime);
    float impulse = quaImpulse(20.0,
        pt * 0.25 + 0.05 + 0.4 * logo_random(float(i + 30)));
    float2 initialPos = getParticleInitialPosition(i);
    float2 velocity;
    velocity.x = 0.4 * impulse * sign(logo_random(float(i + 66)) - 0.1) *
                 (0.3 + 0.6 * logo_random(float(i + 100)));
    velocity.y = 0.8 * impulse * sign(initialPos.y) *
                 (0.1 + 0.9 * logo_random(float(i + 62)));
    return initialPos + getParticleAlive(i, iTime) * velocity * pt +
           float2(textPos.x, 0.5);
}

static float getParticles(float2 uv, float iTime)
{
    float c = 0.0;
    for (int j = 0; j < numParticles; j++) {
        float2 pos = getParticlePosition(j, iTime);
        float d = distance(uv, pos);
        c += (1.0 - smoothstep(0.004, 0.00835, d)) *
             getParticleIntensity(j, iTime);
    }

    return c;
}

fragment float4 logo_fs(VSOut in [[stage_in]],
                        constant FragUniforms &u [[buffer(0)]],
                        texture2d<float> tex [[texture(0)]],
                        sampler smp [[sampler(0)]])
{
    float iTime = u.time;
    float4 fgColor = u.color_primary;
    float4 bgColor = u.color_fill;

    // The GL shader derives uv from gl_FragCoord/512 with a fullscreen
    // 512x512 quad; screen01 carries the same bottom-up normalized
    // coordinate independent of target row order.
    float2 uv = in.screen01;
    float scale = 1.4;
    uv -= 0.5 * (1.0 - 1.0 / scale);
    uv *= scale;
    float2 pos = uv;

    float3 msd = tex.sample(smp, pos).rgb;
    float sd = logo_median(msd.r, msd.g, msd.b);
    float screenPxDistance = pxRange * (sd - 0.5);
    float opacity = clamp(screenPxDistance + 0.5, 0.0, 1.0);
    float4 fill_color = mix(bgColor, fgColor, opacity);
    float outline = clamp(screenPxDistance + 1.6, 0.0, 1.0);
    outline -= clamp(screenPxDistance - 1.6, 0.0, 1.0);
    outline = smoothstep(0.5, 1.0, outline);

    float4 line_color = mix(bgColor, fgColor, outline);
    float4 out_color =
        mix(fill_color, line_color, getSweepingLine(uv, iTime));
    float mask_rhs =
        clamp(sign(uv.x - lineWidth - getSweepingLinePos(iTime)), 0.0, 1.0);
    out_color += fill_color * mask_rhs * getSpotlights(uv, iTime);
    out_color += mix(float4(0.0), fgColor, getParticles(uv, iTime));
    out_color += 2.0 * fgColor * getBox(uv, textPos.x, textPos.z) *
                 getGradients(uv, iTime);
    return out_color;
}
)MSL";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

typedef struct VertUniforms {
    float scale_offset[4];
    float tex_scale_offset[4];
    uint32_t flip_y;
    uint32_t flip_ndc;
    uint32_t pad0;
    uint32_t pad1;
} VertUniforms;

typedef struct FragUniforms {
    float color_primary[4];
    float color_secondary[4];
    float color_fill[4];
    float time;
    float scale;
    float pad0;
    float pad1;
} FragUniforms;

enum FragKind {
    FRAG_MASK = 0,
    FRAG_GAMMA,
    FRAG_LOGO,
    FRAG_KIND_COUNT,
};

enum TargetFmt {
    FMT_RGBA = 0, // offscreen targets
    FMT_BGRA,     // drawable / IOSurface
    FMT_COUNT,
};

static id<MTLDevice> g_dev;
static id<MTLLibrary> g_library;
static id<MTLFunction> g_vert_fn;
static id<MTLFunction> g_frag_fns[FRAG_KIND_COUNT];
static id<MTLRenderPipelineState>
    g_pipelines[FRAG_KIND_COUNT][3 /* MetalBlendMode */][FMT_COUNT];
static id<MTLSamplerState> g_sampler_decal;  // linear, clamp-to-border
static id<MTLSamplerState> g_sampler_linear; // linear, clamp-to-edge
static id<MTLSamplerState> g_sampler_nearest;

struct MetalFbo {
    id<MTLTexture> tex;
    int w, h;
    id<MTLCommandBuffer> cmdbuf;
    id<MTLRenderCommandEncoder> encoder;
};

// Current decal-draw destination. NULL = the main drawable pass.
static MetalFbo *g_current_fbo;

// Decal context (mirrors the GL useProgram/bindTexture/blendFunc state)
static MetalDecalKind g_decal_kind;
static MetalBlendMode g_decal_blend;
static id<MTLTexture> g_decal_tex;
static uint32_t g_decal_time_ms;

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

static MTLPixelFormat fmt_to_mtl(enum TargetFmt fmt)
{
    return fmt == FMT_RGBA ? MTLPixelFormatRGBA8Unorm
                           : MTLPixelFormatBGRA8Unorm;
}

static id<MTLRenderPipelineState> get_pipeline(enum FragKind kind,
                                               MetalBlendMode blend,
                                               enum TargetFmt fmt)
{
    int b = (int)blend;
    if (g_pipelines[kind][b][fmt]) {
        return g_pipelines[kind][b][fmt];
    }

    MTLRenderPipelineDescriptor *desc =
        [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = g_vert_fn;
    desc.fragmentFunction = g_frag_fns[kind];
    MTLRenderPipelineColorAttachmentDescriptor *att = desc.colorAttachments[0];
    att.pixelFormat = fmt_to_mtl(fmt);

    switch (blend) {
    case MetalBlendMode::None:
        att.blendingEnabled = NO;
        break;
    case MetalBlendMode::Cutout:
        att.blendingEnabled = YES;
        att.rgbBlendOperation = MTLBlendOperationAdd;
        att.alphaBlendOperation = MTLBlendOperationAdd;
        att.sourceRGBBlendFactor = MTLBlendFactorOneMinusDestinationAlpha;
        att.destinationRGBBlendFactor = MTLBlendFactorOne;
        att.sourceAlphaBlendFactor = MTLBlendFactorOneMinusDestinationAlpha;
        att.destinationAlphaBlendFactor = MTLBlendFactorOne;
        break;
    case MetalBlendMode::Alpha:
        att.blendingEnabled = YES;
        att.rgbBlendOperation = MTLBlendOperationAdd;
        att.alphaBlendOperation = MTLBlendOperationAdd;
        att.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        att.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        att.sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
        att.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        break;
    }

    NSError *err = nil;
    id<MTLRenderPipelineState> pso =
        [g_dev newRenderPipelineStateWithDescriptor:desc error:&err];
    [desc release];
    if (!pso) {
        fprintf(stderr, "metal-helpers: pipeline creation failed: %s\n",
                err ? [[err localizedDescription] UTF8String] : "?");
        assert(0);
    }
    g_pipelines[kind][b][fmt] = pso;
    return pso;
}

bool MetalHelpersInit()
{
    g_dev = (id<MTLDevice>)xemu_metal_get_device();
    if (!g_dev) {
        return false;
    }

    NSError *err = nil;
    MTLCompileOptions *opts = [[MTLCompileOptions alloc] init];
    g_library = [g_dev newLibraryWithSource:kShaderSource
                                    options:opts
                                      error:&err];
    [opts release];
    if (!g_library) {
        fprintf(stderr, "metal-helpers: shader compile failed: %s\n",
                err ? [[err localizedDescription] UTF8String] : "?");
        return false;
    }

    g_vert_fn = [g_library newFunctionWithName:@"decal_vs"];
    g_frag_fns[FRAG_MASK] = [g_library newFunctionWithName:@"mask_fs"];
    g_frag_fns[FRAG_GAMMA] = [g_library newFunctionWithName:@"gamma_fs"];
    g_frag_fns[FRAG_LOGO] = [g_library newFunctionWithName:@"logo_fs"];
    assert(g_vert_fn && g_frag_fns[FRAG_MASK] && g_frag_fns[FRAG_GAMMA] &&
           g_frag_fns[FRAG_LOGO]);

    MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = MTLSamplerAddressModeClampToBorderColor;
    sd.tAddressMode = MTLSamplerAddressModeClampToBorderColor;
    sd.borderColor = MTLSamplerBorderColorTransparentBlack;
    g_sampler_decal = [g_dev newSamplerStateWithDescriptor:sd];

    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    g_sampler_linear = [g_dev newSamplerStateWithDescriptor:sd];

    sd.minFilter = MTLSamplerMinMagFilterNearest;
    sd.magFilter = MTLSamplerMinMagFilterNearest;
    g_sampler_nearest = [g_dev newSamplerStateWithDescriptor:sd];
    [sd release];

    return true;
}

const char *MetalGetDeviceName()
{
    return xemu_metal_device_name();
}

// ---------------------------------------------------------------------------
// ImGui bridge
// ---------------------------------------------------------------------------

bool MetalImGuiInit()
{
    return ImGui_ImplMetal_Init((id<MTLDevice>)xemu_metal_get_device());
}

void MetalImGuiShutdown()
{
    ImGui_ImplMetal_Shutdown();
}

void MetalImGuiNewFrame()
{
    ImGui_ImplMetal_NewFrame(
        (MTLRenderPassDescriptor *)xemu_metal_get_render_pass_desc());
}

void MetalImGuiRenderDrawData(ImDrawData *draw_data)
{
    ImGui_ImplMetal_RenderDrawData(
        draw_data, (id<MTLCommandBuffer>)xemu_metal_get_command_buffer(),
        (id<MTLRenderCommandEncoder>)xemu_metal_get_render_encoder());
}

void MetalImGuiCreateFontsTexture()
{
    ImGui_ImplMetal_CreateFontsTexture((id<MTLDevice>)xemu_metal_get_device());
}

void MetalImGuiDestroyFontsTexture()
{
    ImGui_ImplMetal_DestroyFontsTexture();
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

uintptr_t MetalCreateTextureFromRgba(const unsigned char *rgba, int w, int h)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                     width:w
                                    height:h
                                 mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [g_dev newTextureWithDescriptor:td];
    assert(tex != nil);
    [tex replaceRegion:MTLRegionMake2D(0, 0, w, h)
           mipmapLevel:0
             withBytes:rgba
           bytesPerRow:(NSUInteger)w * 4];
    return (uintptr_t)tex; // retained (+1 from newTexture)
}

uintptr_t MetalLoadTextureFromMemory(const unsigned char *buf,
                                     unsigned int size, bool flip)
{
    // Match LoadTextureFromMemory in gl-helpers.cc: flip vertically by
    // default so decal atlas coordinates work identically.
    stbi_set_flip_vertically_on_load(flip);

    int width, height, channels = 0;
    unsigned char *data =
        stbi_load_from_memory(buf, size, &width, &height, &channels, 4);
    assert(data != NULL);

    uintptr_t tex = MetalCreateTextureFromRgba(data, width, height);
    stbi_image_free(data);
    return tex;
}

void MetalDestroyTexture(uintptr_t tex)
{
    if (tex) {
        [(id<MTLTexture>)tex release];
    }
}

void MetalRetainTexture(uintptr_t tex)
{
    if (tex) {
        [(id<MTLTexture>)tex retain];
    }
}

void MetalTextureDims(uintptr_t tex, int *w, int *h)
{
    id<MTLTexture> t = (id<MTLTexture>)tex;
    if (w) {
        *w = t ? (int)t.width : 0;
    }
    if (h) {
        *h = t ? (int)t.height : 0;
    }
}

// ---------------------------------------------------------------------------
// Offscreen targets (Fbo equivalent)
// ---------------------------------------------------------------------------

MetalFbo *MetalFboCreate(int w, int h)
{
    MetalFbo *fbo = new MetalFbo();
    fbo->w = w;
    fbo->h = h;

    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                     width:w
                                    height:h
                                 mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    fbo->tex = [g_dev newTextureWithDescriptor:td];
    assert(fbo->tex != nil);
    return fbo;
}

void MetalFboDestroy(MetalFbo *fbo)
{
    if (!fbo) {
        return;
    }
    [fbo->tex release];
    delete fbo;
}

uintptr_t MetalFboTexture(MetalFbo *fbo)
{
    return (uintptr_t)fbo->tex;
}

void MetalFboTarget(MetalFbo *fbo)
{
    assert(g_current_fbo == NULL && "nested Fbo targets unsupported");

    // Offscreen passes get their own command buffer, committed in
    // Restore(). Queue submission order guarantees they execute before
    // the main present command buffer (committed later), so the main
    // pass can sample the result — same immediacy semantics as the GL
    // FBO path.
    id<MTLCommandQueue> queue =
        (id<MTLCommandQueue>)xemu_metal_get_command_queue();
    fbo->cmdbuf = [[queue commandBuffer] retain];

    MTLRenderPassDescriptor *rpd =
        [MTLRenderPassDescriptor renderPassDescriptor];
    rpd.colorAttachments[0].texture = fbo->tex;
    rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
    rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
    rpd.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);

    fbo->encoder =
        [[fbo->cmdbuf renderCommandEncoderWithDescriptor:rpd] retain];
    g_current_fbo = fbo;
}

void MetalFboRestore(MetalFbo *fbo)
{
    assert(g_current_fbo == fbo);
    [fbo->encoder endEncoding];
    [fbo->cmdbuf commit];
    [fbo->encoder release];
    fbo->encoder = nil;
    [fbo->cmdbuf release];
    fbo->cmdbuf = nil;
    g_current_fbo = NULL;
}

// ---------------------------------------------------------------------------
// Decal rendering
// ---------------------------------------------------------------------------

void MetalDecalBegin(MetalDecalKind kind, uintptr_t tex)
{
    g_decal_kind = kind;
    g_decal_tex = (id<MTLTexture>)tex;
    g_decal_blend = MetalBlendMode::None;
}

void MetalDecalBlend(MetalBlendMode mode)
{
    g_decal_blend = mode;
}

void MetalDecalSetTime(uint32_t time_ms)
{
    g_decal_time_ms = time_ms;
}

void MetalDecalEnd()
{
    g_decal_tex = nil;
}

static id<MTLRenderCommandEncoder> current_encoder(int *out_w, int *out_h,
                                                   bool *out_flip_ndc,
                                                   enum TargetFmt *out_fmt)
{
    if (g_current_fbo) {
        *out_w = g_current_fbo->w;
        *out_h = g_current_fbo->h;
        *out_flip_ndc = true; // match GL FBO row order
        *out_fmt = FMT_RGBA;
        return g_current_fbo->encoder;
    }
    *out_w = xemu_metal_get_drawable_width();
    *out_h = xemu_metal_get_drawable_height();
    *out_flip_ndc = false;
    *out_fmt = FMT_BGRA;
    return (id<MTLRenderCommandEncoder>)xemu_metal_get_render_encoder();
}

void MetalRenderDecal(float x, float y, float w, float h, float tex_x,
                      float tex_y, float tex_w, float tex_h, uint32_t primary,
                      uint32_t secondary, uint32_t fill)
{
    int vw, vh;
    bool flip_ndc;
    enum TargetFmt fmt;
    id<MTLRenderCommandEncoder> enc =
        current_encoder(&vw, &vh, &flip_ndc, &fmt);
    if (!enc || !g_decal_tex) {
        return;
    }

    // Match the int truncation in the GL RenderDecal
    x = (int)x;
    y = (int)y;
    w = (int)w;
    h = (int)h;
    tex_x = (int)tex_x;
    tex_y = (int)tex_y;
    tex_w = (int)tex_w;
    tex_h = (int)tex_h;

    float ww = vw, wh = vh;
    float tw = (float)g_decal_tex.width, th = (float)g_decal_tex.height;

    VertUniforms vu = {};
    vu.scale_offset[0] = w / ww;
    vu.scale_offset[1] = h / wh;
    vu.scale_offset[2] = -1 + ((2 * x + w) / ww);
    vu.scale_offset[3] = -1 + ((2 * y + h) / wh);
    vu.tex_scale_offset[0] = tex_w / tw;
    vu.tex_scale_offset[1] = tex_h / th;
    vu.tex_scale_offset[2] = tex_x / tw;
    vu.tex_scale_offset[3] = tex_y / th;
    vu.flip_y = 0;
    vu.flip_ndc = flip_ndc ? 1 : 0;

#define COL(color, c) (float)(((color) >> ((c) * 8)) & 0xff) / 255.0f
    FragUniforms fu = {};
    fu.color_primary[0] = COL(primary, 3);
    fu.color_primary[1] = COL(primary, 2);
    fu.color_primary[2] = COL(primary, 1);
    fu.color_primary[3] = COL(primary, 0);
    fu.color_secondary[0] = COL(secondary, 3);
    fu.color_secondary[1] = COL(secondary, 2);
    fu.color_secondary[2] = COL(secondary, 1);
    fu.color_secondary[3] = COL(secondary, 0);
    fu.color_fill[0] = COL(fill, 3);
    fu.color_fill[1] = COL(fill, 2);
    fu.color_fill[2] = COL(fill, 1);
    fu.color_fill[3] = COL(fill, 0);
    fu.time = g_decal_time_ms / 1000.0f;
#undef COL

    enum FragKind kind =
        g_decal_kind == MetalDecalKind::Logo ? FRAG_LOGO : FRAG_MASK;
    [enc setRenderPipelineState:get_pipeline(kind, g_decal_blend, fmt)];
    [enc setVertexBytes:&vu length:sizeof(vu) atIndex:0];
    [enc setFragmentBytes:&fu length:sizeof(fu) atIndex:0];
    [enc setFragmentTexture:g_decal_tex atIndex:0];
    [enc setFragmentSamplerState:g_sampler_decal atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip
            vertexStart:0
            vertexCount:4];
}

// ---------------------------------------------------------------------------
// Game-frame gamma blit
// ---------------------------------------------------------------------------

#include "config-host.h"

extern "C" {
const uint8_t *nv2a_get_dac_palette(void);
int nv2a_get_screen_off(void);
#ifdef CONFIG_VULKAN
/* Async MetalFX ordering (hw/xbox/nv2a/pgraph/vk/metalfx_upscale.m).
 * The provider lives in the Vulkan renderer; a darwin build without
 * Vulkan (no usable MoltenVK at configure time) has no MetalFX ring
 * producer, so there is nothing to order against. */
void *metalfx_present_event(void);
uint64_t metalfx_present_event_last_value(void);
#endif
}

static void render_framebuffer_to(id<MTLRenderCommandEncoder> enc,
                                  enum TargetFmt fmt, bool flip_ndc,
                                  uintptr_t tex, bool flip,
                                  const float scale[2], bool nearest)
{
    id<MTLTexture> t = (id<MTLTexture>)tex;
    if (!enc || !t) {
        return;
    }

    VertUniforms vu = {};
    vu.scale_offset[0] = scale[0];
    vu.scale_offset[1] = scale[1];
    vu.scale_offset[2] = 0;
    vu.scale_offset[3] = 0;
    vu.tex_scale_offset[0] = 1.0f;
    vu.tex_scale_offset[1] = 1.0f;
    vu.tex_scale_offset[2] = 0;
    vu.tex_scale_offset[3] = 0;
    vu.flip_y = flip ? 1 : 0;
    vu.flip_ndc = flip_ndc ? 1 : 0;

    uint32_t palette_packed[256];
    const uint8_t *palette = nv2a_get_dac_palette();
    for (int i = 0; i < 256; i++) {
        palette_packed[i] = ((uint32_t)palette[i * 3 + 2] << 16) |
                            ((uint32_t)palette[i * 3 + 1] << 8) |
                            (uint32_t)palette[i * 3];
    }

    [enc setRenderPipelineState:get_pipeline(FRAG_GAMMA, MetalBlendMode::None,
                                             fmt)];
    [enc setVertexBytes:&vu length:sizeof(vu) atIndex:0];
    [enc setFragmentBytes:palette_packed length:sizeof(palette_packed)
                  atIndex:0];
    [enc setFragmentTexture:t atIndex:0];
    [enc setFragmentSamplerState:(nearest ? g_sampler_nearest
                                          : g_sampler_linear)
                         atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip
            vertexStart:0
            vertexCount:4];
}

void MetalRenderFramebuffer(uintptr_t tex, int width, int height, bool flip,
                            const float scale[2], bool nearest)
{
    (void)width;
    (void)height; // drawable pass viewport covers the full target

    if (nv2a_get_screen_off()) {
        return; // pass was cleared at begin_frame
    }

    id<MTLRenderCommandEncoder> enc =
        (id<MTLRenderCommandEncoder>)xemu_metal_get_render_encoder();
    render_framebuffer_to(enc, FMT_BGRA, false, tex, flip, scale, nearest);
}

bool MetalRenderFramebufferToRgb(uintptr_t tex, bool flip, int width,
                                 int height, uint8_t *rgb_out)
{
    if (!tex || width <= 0 || height <= 0) {
        return false;
    }

    /* May run outside the per-frame pool (savestate thumbnail path). */
    @autoreleasepool {

    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                     width:width
                                    height:height
                                 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> target = [g_dev newTextureWithDescriptor:td];
    if (!target) {
        return false;
    }

    id<MTLCommandQueue> queue =
        (id<MTLCommandQueue>)xemu_metal_get_command_queue();
    id<MTLCommandBuffer> cb = [queue commandBuffer];

    /* The source may be an in-flight MetalFX ring texture (produced on
     * a different queue with async signaling); order against it. */
#ifdef CONFIG_VULKAN
    id<MTLSharedEvent> ev = (id<MTLSharedEvent>)metalfx_present_event();
    if (ev) {
        [cb encodeWaitForEvent:ev value:metalfx_present_event_last_value()];
    }
#endif

    MTLRenderPassDescriptor *rpd =
        [MTLRenderPassDescriptor renderPassDescriptor];
    rpd.colorAttachments[0].texture = target;
    rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
    rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
    rpd.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);

    id<MTLRenderCommandEncoder> enc =
        [cb renderCommandEncoderWithDescriptor:rpd];
    const float scale[2] = { 1.0f, 1.0f };
    /*
     * Orientation: the present path (flip_ndc=false, flip as given)
     * displays correctly, which fixes the texture's row convention
     * (NV2A compositor output = GL-style, row 0 at the image
     * bottom). This offscreen pass adds a flip_ndc inversion to
     * replicate GL FBO row order, so the texture-coordinate
     * direction must be inverted too — pass `!flip`, exactly like
     * the GL readback path below. Passing `flip` unmirrored left
     * savestate thumbnails and screenshots upside down under the
     * Metal backend.
     */
    render_framebuffer_to(enc, FMT_RGBA, true, tex, !flip, scale, false);
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];

    size_t stride = (size_t)width * 4;
    uint8_t *rgba = (uint8_t *)malloc(stride * height);
    if (!rgba) {
        [target release];
        return false;
    }
    [target getBytes:rgba
         bytesPerRow:stride
          fromRegion:MTLRegionMake2D(0, 0, width, height)
         mipmapLevel:0];
    [target release];

    for (int yy = 0; yy < height; yy++) {
        const uint8_t *src = rgba + (size_t)yy * stride;
        uint8_t *dst = rgb_out + (size_t)yy * width * 3;
        for (int xx = 0; xx < width; xx++) {
            dst[xx * 3 + 0] = src[xx * 4 + 0];
            dst[xx * 3 + 1] = src[xx * 4 + 1];
            dst[xx * 3 + 2] = src[xx * 4 + 2];
        }
    }
    free(rgba);
    return true;

    } // @autoreleasepool
}

#endif // __APPLE__
