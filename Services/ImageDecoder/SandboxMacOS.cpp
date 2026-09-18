/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <ImageDecoder/Sandbox.h>
#include <LibSandbox/Sandbox.h>

namespace ImageDecoder {

ErrorOr<void> apply_sandbox()
{
    TRY(Sandbox::configure_runtime());

    return Sandbox::apply_macos_sandbox({});
}

}
