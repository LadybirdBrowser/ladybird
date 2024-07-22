/*
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <LibGfx/Font/OpenType/Typeface.h>

namespace WOFF {

ErrorOr<NonnullRefPtr<OpenType::Typeface>> try_load_from_resource(Core::Resource const&, unsigned index = 0);
ErrorOr<NonnullRefPtr<OpenType::Typeface>> try_load_from_externally_owned_memory(ReadonlyBytes bytes, unsigned index = 0);

}
