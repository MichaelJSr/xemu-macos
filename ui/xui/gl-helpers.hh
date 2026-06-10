//
// xemu User Interface
//
// Copyright (C) 2020-2022 Matt Borgerson
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
#pragma once
#include <stdint.h>
#include <vector>
#include "common.hh"
#include "../xemu-input.h"

#ifdef __APPLE__
struct MetalFbo;
#endif

/*
 * Texture handles passed around the UI are uintptr_t: a GLuint under
 * the OpenGL presentation backend, a retained id<MTLTexture> pointer
 * under the Metal backend. Both forms are used directly as
 * ImTextureID.
 */

class Fbo
{
public:
    static GLint vp[4];
    static GLint original_fbo;
    static bool blend;

    int w, h;
    GLuint fbo, tex;
#ifdef __APPLE__
    MetalFbo *mfbo;
#endif

    Fbo(int width, int height);
    ~Fbo();
    uintptr_t Texture();
    void Target();
    void Restore();
};

extern Fbo *controller_fbo, *xmu_fbo, *logo_fbo;
extern uintptr_t g_icon_tex;

void InitCustomRendering(void);
void RenderLogo(uint32_t time);
void RenderController(float frame_x, float frame_y, uint32_t primary_color,
                      uint32_t secondary_color, ControllerState *state);
void RenderControllerPort(float frame_x, float frame_y, int i,
                          uint32_t port_color);
void RenderXmu(float frame_x, float frame_y, uint32_t primary_color,
               uint32_t secondary_color);
extern "C" void xemu_set_framebuffer_texture_is_rect(bool is_rect);
void RenderFramebuffer(uintptr_t tex, int width, int height, bool flip);
void RenderFramebuffer(uintptr_t tex, int width, int height, bool flip, float scale[2]);
bool RenderFramebufferToPng(uintptr_t tex, bool flip, std::vector<uint8_t> &png, int max_width = 0, int max_height = 0);
void SaveScreenshot(uintptr_t tex, bool flip);
void ScaleDimensions(int src_width, int src_height, int max_width, int max_height, int *out_width, int *out_height);

// Load an image (PNG/JPG via stb) into a backend texture handle.
uintptr_t LoadUiTextureFromMemory(const unsigned char *buf, unsigned int size,
                                  bool flip = true);
uintptr_t CreateUiTextureFromRgba(const unsigned char *rgba, int w, int h);
void DestroyUiTexture(uintptr_t tex);
void GetUiTextureDims(uintptr_t tex, int *w, int *h);
