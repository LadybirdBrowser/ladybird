/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>

namespace Sandbox {

[[nodiscard]] ErrorOr<void> install_no_new_privileges();
[[nodiscard]] ErrorOr<void> configure_runtime();
[[nodiscard]] ErrorOr<void> restrict_filesystem_with_landlock();

}
