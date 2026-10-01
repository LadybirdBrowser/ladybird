/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>

namespace Web::HTML {

// Names one environment settings object in this process; no other settings object ever has the same address.
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u64, EnvironmentSettingsObjectAddress);

}
