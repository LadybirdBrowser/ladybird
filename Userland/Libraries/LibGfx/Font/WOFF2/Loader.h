/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <LibGfx/Font/Typeface.h>

namespace WOFF2 {

ErrorOr<NonnullRefPtr<Gfx::Typeface>> try_load_from_externally_owned_memory(ReadonlyBytes);

}
