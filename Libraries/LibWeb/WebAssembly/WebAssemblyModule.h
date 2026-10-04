/*
 * Copyright (c) 2025, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/HostModule.h>

namespace Web::WebAssembly {

// https://webassembly.github.io/esm-integration/js-api/index.html#parse-a-webassembly-module
// The result is a WebAssembly Module Record: a host module whose abstract methods are those of WebAssembly modules.
JS::ThrowCompletionOr<GC::Ref<JS::HostModule>> parse_a_webassembly_module(ByteBuffer bytes, JS::Realm&, StringView filename = {}, GC::Ptr<GC::Cell> host_defined = nullptr);

}
