// ftpd is a server implementation based on the following:
// - RFC  959 (https://tools.ietf.org/html/rfc959)
// - RFC 3659 (https://tools.ietf.org/html/rfc3659)
// - suggested implementation details from https://cr.yp.to/ftp/filesystem.html
// - Deflate transmission mode for FTP
//   (https://tools.ietf.org/html/draft-preston-ftpext-deflate-04)
//
// The MIT License (MIT)
//
// Copyright (C) 2024 Michael Theall
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#ifndef CLASSIC
#include <citro3d.h>

struct ImDrawList;
struct ImDrawCmd;

namespace imgui
{
namespace citro3d
{
/// \brief Initialize citro3d
void init ();
/// \brief Deinitialize citro3d
void exit ();

/// \brief Render ImGui draw list
/// \param topLeft_ Top left render target
/// \param topRight_ Top right render target (skipped if not stereoscopic)
/// \param bottom_ Bottom render target
void render (C3D_RenderTarget *topLeft_, C3D_RenderTarget *topRight_, C3D_RenderTarget *bottom_);

/// \brief Set Z offset (for stereoscopic effect)
/// \param drawList_ Draw list
/// \param drawCmd_ Draw command
void setZ (ImDrawList const *drawList_, ImDrawCmd const *drawCmd_);
}
}
#endif
