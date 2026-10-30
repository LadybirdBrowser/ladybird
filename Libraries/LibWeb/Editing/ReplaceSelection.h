/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <LibWeb/Forward.h>

namespace Web::Editing {

void replace_selection_with_fragment(DOM::Document&, Utf16View html);

}
