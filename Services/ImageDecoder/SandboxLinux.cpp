/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <ImageDecoder/Sandbox.h>
#include <LibSandbox/Sandbox.h>
#include <LibSandbox/Seccomp.h>

namespace ImageDecoder {

ErrorOr<void> apply_sandbox()
{
    TRY(Sandbox::install_no_new_privileges());
    TRY(Sandbox::configure_runtime());
    TRY(Sandbox::restrict_filesystem_with_landlock());

    Sandbox::SeccompPolicy policy;
    policy.deny_readonly_filesystem_probes();
    policy.allow_file_descriptor_operations();
    policy.allow_ipc();
    policy.allow_common_runtime();
    TRY(policy.install());

    return {};
}

}
