/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Root.h>
#include <LibWeb/Forward.h>

namespace Web::WebGL {

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLintptr = long long;

// FIXME: This should really be "struct __GLsync*", but the linker doesn't recognise it.
//        Since this conflicts with the original definition of GLsync, the suffix "Internal" has been added.
using GLsyncInternal = void*;

}
