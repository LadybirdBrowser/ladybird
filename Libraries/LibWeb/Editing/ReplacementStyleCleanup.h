/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>

namespace Web::Editing {

class InsertedContent;

// Remove inline declarations which merely repeat the inserted element's cascade context. Clipboard serializers use
// such declarations to transport rendered appearance between documents, but retaining them after insertion would
// make the pasted content unnecessarily override its new document.
void remove_redundant_styles_from_inserted_content(InsertedContent&);

}
