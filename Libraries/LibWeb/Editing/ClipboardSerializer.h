/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWeb/Forward.h>

namespace Web::Editing {

Utf16String serialize_range_as_plain_text_for_clipboard(DOM::Range const&);
Utf16String serialize_range_as_html_for_clipboard(DOM::Range&);

}
