/*
 * Copyright (c) 2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/WOFF2/Loader.h>
#include <stddef.h>

extern "C" int LLVMFuzzerTestOneInput(u8 const* data, size_t size)
{
    AK::set_debug_enabled(false);
    (void)WOFF2::try_load_from_externally_owned_memory({ data, size });
    return 0;
}
