/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/DistinctNumeric.h>
#include <LibWebCommon/Page/PageId.h>

namespace Compositing {

AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, CompositorContextId);

inline CompositorContextId compositor_context_id_for_page(Compositing::PageId page_id)
{
    VERIFY(page_id.value() > 0);
    return CompositorContextId { page_id.value() };
}

enum class PagePresentationRegistration {
    No,
    Yes,
};

}
