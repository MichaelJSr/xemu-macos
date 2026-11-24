//
// xemu User Interface - Game patches window
//
// A standalone front end for game-patches: pick a disc image, collect
// .xbe/.ips/.bps/.jmp files in its per-title patch folder, and build a
// patched qcow2 overlay of the image to load in the DVD drive. The disc
// image itself is never modified.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once

// Draw the window (call every frame)
void GamePatchesWindowDraw(void);
void GamePatchesOpenWindow(void);
