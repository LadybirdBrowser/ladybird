/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCompositing/WebGL/GLFunctions.h>
#include <LibCompositing/WebGL/TextureUpload.h>
#include <LibCompositing/WebGL/Types.h>
#include <LibCompositing/WebGL/WebGLCommandList.h>
#include <LibCompositing/WebGL/WebGLSharedCommandBuffer.h>

// The WebGL objects LibWeb implements speak the command stream LibCompositing defines.

namespace Web::WebGL {

namespace Commands = Compositing::WebGL::Commands;
namespace SyncCalls = Compositing::WebGL::SyncCalls;

using Compositing::WebGL::GLchar;
using Compositing::WebGL::GLenum;
using Compositing::WebGL::GLFunctions;
using Compositing::WebGL::GLint;
using Compositing::WebGL::GLintptr;
using Compositing::WebGL::GLsizei;
using Compositing::WebGL::GLsyncInternal;
using Compositing::WebGL::GLuint;
using Compositing::WebGL::is_valid_2d_pixel_unpack_state;
using Compositing::WebGL::max_webgl_drawing_buffer_dimension;
using Compositing::WebGL::PixelUnpackState;
using Compositing::WebGL::ReadPixelsResult;
using Compositing::WebGL::required_2d_texture_data_size;
using Compositing::WebGL::texture_export_format;
using Compositing::WebGL::to_string;
using Compositing::WebGL::WebGLCommandHeader;
using Compositing::WebGL::WebGLCommandList;
using Compositing::WebGL::WebGLCommandType;
using Compositing::WebGL::WebGLDataSpan;
using Compositing::WebGL::WebGLObjectId;
using Compositing::WebGL::WebGLSharedCommandBuffer;
using Compositing::WebGL::WebGLSyncCall;
using Compositing::WebGL::WebGLSyncCallHeader;
using Compositing::WebGL::WebGLSyncCallType;
using Compositing::WebGL::WebGLVersion;

}
