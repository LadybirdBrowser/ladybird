/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/IterationDecision.h>
#include <AK/Optional.h>
#include <LibWeb/Forward.h>

namespace Web {

size_t find_line_start(Utf16View const&, size_t offset);
size_t find_line_end(Utf16View const&, size_t offset);

}
