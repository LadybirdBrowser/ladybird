/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::DOM {

class Range;

}

namespace Web::Editing {

WebIDL::ExceptionOr<Utf16String> sanitize_clipboard_html(DOM::Range&, Utf16View);

}
