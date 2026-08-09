/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>

namespace Web::CSS {

AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u64, StyleRecordID, CastToBool, CastToUnderlying);

}
