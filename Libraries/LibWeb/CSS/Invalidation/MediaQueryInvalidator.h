/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>

namespace Web::CSS::Invalidation {

void evaluate_media_rules_and_publish_conditions(DOM::Document&);

}
