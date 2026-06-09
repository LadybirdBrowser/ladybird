/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/StringView.h>

namespace RendererSandbox {

[[nodiscard]] ErrorOr<void> apply_sandbox(Optional<StringView> config_path);

}
