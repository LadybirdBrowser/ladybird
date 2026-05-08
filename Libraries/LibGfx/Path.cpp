/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Path.h>
#include <LibGfx/PathSkia.h>

namespace Gfx {

Path Path::from_serialized_bytes(ReadonlyBytes bytes)
{
    Gfx::Path path;
    path.impl().deserialize_from_bytes(bytes);
    return path;
}

NonnullOwnPtr<Gfx::PathImpl> PathImpl::create()
{
    return PathImplSkia::create();
}

PathImpl::~PathImpl() = default;

}
