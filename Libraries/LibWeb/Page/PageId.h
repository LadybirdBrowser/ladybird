/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>

namespace Web {

// Identifies a page within one WebContent process. The UI process hands out page IDs and tracks which
// connection each one belongs to, so a page ID in a message from a helper process is a claim about the
// sender. The distinct type keeps that claim from being mistaken for an ordinary number.
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, PageId);

}
