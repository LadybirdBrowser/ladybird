/*
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <LibGfx/Font/Typeface.h>

namespace WOFF {

ErrorOr<NonnullRefPtr<Gfx::Typeface>> try_load_from_resource(Core::Resource const&, unsigned index = 0);
ErrorOr<NonnullRefPtr<Gfx::Typeface>> try_load_from_externally_owned_memory(ReadonlyBytes bytes, unsigned index = 0);

}
