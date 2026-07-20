/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/TrustedTypes/TrustedHTML.h>

namespace Web::Editing {

void replace_selection_with_fragment(DOM::Document&, TrustedTypes::TrustedHTMLOrString const&);

}
