/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCompositing/KeyCode.h>

namespace Web::UIEvents {

using Compositing::KeyCode;
using enum Compositing::KeyCode;
using Compositing::KeyModifier;
using enum Compositing::KeyModifier;
using Compositing::code_point_to_key_code;
using Compositing::key_code_from_string;

}
