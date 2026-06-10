//
// xemu User Interface — Metal presentation helpers
//
// C++-callable interface to the Metal UI compositor (implemented in
// metal-helpers.mm, Objective-C++). Mirrors the OpenGL helpers in
// gl-helpers.cc for the Metal presentation backend: decal rendering,
// offscreen render targets, the game-frame gamma blit, texture
// management, CPU readback for captures, and the ImGui Metal bridge.
//
// Texture handles cross this interface as uintptr_t holding a
// retained id<MTLTexture>; the same value is used directly as
// ImTextureID (the ImGui Metal backend interprets ImTextureID as an
// MTLTexture pointer).
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
#ifndef XUI_METAL_HELPERS_HH
#define XUI_METAL_HELPERS_HH

#ifdef __APPLE__

#include <stdint.h>

struct ImDrawData;

// Lifecycle (requires xemu_metal_init() to have succeeded)
bool MetalHelpersInit();
void MetalHelpersShutdown();

// ImGui Metal bridge
bool MetalImGuiInit();
void MetalImGuiShutdown();
void MetalImGuiNewFrame();
void MetalImGuiRenderDrawData(ImDrawData *draw_data);
void MetalImGuiCreateFontsTexture();
void MetalImGuiDestroyFontsTexture();

// Texture management. Returned handles are retained id<MTLTexture>.
uintptr_t MetalCreateTextureFromRgba(const unsigned char *rgba, int w, int h);
uintptr_t MetalLoadTextureFromMemory(const unsigned char *buf,
                                     unsigned int size, bool flip);
void MetalDestroyTexture(uintptr_t tex);
void MetalTextureDims(uintptr_t tex, int *w, int *h);

// Offscreen render target (Fbo equivalent). Content row order matches
// GL FBO conventions (row 0 = scene bottom) so existing ImGui UV
// flips at call sites work unchanged.
struct MetalFbo;
MetalFbo *MetalFboCreate(int w, int h);
void MetalFboDestroy(MetalFbo *fbo);
uintptr_t MetalFboTexture(MetalFbo *fbo);
void MetalFboTarget(MetalFbo *fbo);
void MetalFboRestore(MetalFbo *fbo);

// Decal rendering context (parallels the GL shader/texture/blend
// state calls in gl-helpers.cc). Draws target the current MetalFbo
// between Target/Restore, or the main drawable pass otherwise.
enum class MetalDecalKind {
    Mask,
    Logo,
};
enum class MetalBlendMode {
    None,   // glBlendFunc(GL_ONE, GL_ZERO)
    Cutout, // glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_ONE)
    Alpha,  // glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)
};
void MetalDecalBegin(MetalDecalKind kind, uintptr_t tex);
void MetalDecalBlend(MetalBlendMode mode);
void MetalDecalSetTime(uint32_t time_ms);
void MetalDecalEnd();
void MetalRenderDecal(float x, float y, float w, float h, float tex_x,
                      float tex_y, float tex_w, float tex_h,
                      uint32_t primary, uint32_t secondary, uint32_t fill);

// Game-frame gamma blit into the current main drawable pass.
void MetalRenderFramebuffer(uintptr_t tex, int width, int height, bool flip,
                            const float scale[2], bool nearest);

// Render `tex` through the gamma blit into an offscreen target of the
// given size and read back tightly-packed RGB8. Synchronous (used for
// screenshots / savestate thumbnails only).
bool MetalRenderFramebufferToRgb(uintptr_t tex, bool flip, int width,
                                 int height, uint8_t *rgb_out);

const char *MetalGetDeviceName();

#endif // __APPLE__

#endif // XUI_METAL_HELPERS_HH
