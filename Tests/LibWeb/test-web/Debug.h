/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Types.h>

namespace TestWeb {

class TestWebView;
struct Test;

void maybe_attach_on_fail_fast_timeout(pid_t);

void append_timeout_diagnostics_to_stderr(StringBuilder&, TestWebView&, Test const&, size_t view_id);
void append_timeout_backtraces_to_stderr(StringBuilder&, TestWebView&, Test const&, size_t view_id);

StringBuilder convert_ansi_to_html(StringView);

}
