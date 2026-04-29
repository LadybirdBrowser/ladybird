/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::DOM {

class Document;

}

namespace Web::CSS::Invalidation {

void evaluate_media_rules_and_invalidate_style(DOM::Document&);

}
