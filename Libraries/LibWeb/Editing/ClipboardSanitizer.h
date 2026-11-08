/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Editing {

WebIDL::ExceptionOr<GC::Ref<DOM::DocumentFragment>> sanitize_clipboard_html(DOM::Range&, Utf16View);

}
