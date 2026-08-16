/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <LibSandbox/Sandbox.h>
#include <LibWasm/Types.h>
#include <WasmCompiler/Sandbox.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

namespace WasmCompiler {

ErrorOr<void> apply_sandbox()
{
    TRY(Sandbox::configure_runtime());

    auto const& compiler_path = Wasm::cranelift_compiler_path();

    Vector<Sandbox::SeatbeltPath> paths;
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, compiler_path, Sandbox::SeatbeltPath::Access::ReadAndExecute));

    return Sandbox::apply_macos_sandbox(paths.span(), Sandbox::NetworkAccess::Denied, { { compiler_path } });
}

}
