/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Path.h>
#include <LibWeb/HTML/Path2D.h>

namespace Web::HTML {

// Defined in Bindings/Rust/src/html/exports.rs.
extern "C" Gfx::Path const* web_path2d_path(void* path);

Gfx::Path const& Path2D::path() const
{
    return *web_path2d_path(const_cast<Path2D*>(this));
}

}
