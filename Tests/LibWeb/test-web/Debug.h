/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Types.h>

namespace TestWeb {

void maybe_attach_on_fail_fast_timeout(pid_t);

StringBuilder convert_ansi_to_html(StringView);

}
