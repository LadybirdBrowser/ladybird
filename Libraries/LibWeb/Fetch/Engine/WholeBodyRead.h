/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/Variant.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/FetchBody.h>

namespace Web::Fetch::Engine {

struct WholeBodyReadFailure { };
using WholeBodyReadResult = Variant<ByteBuffer, WholeBodyReadFailure>;

// Runs the steps with the whole body's bytes, or with a failure if the channel errors, is cancelled or is transferred.
WEB_API void read_whole_body(Infrastructure::ResponseBody const&, Function<void(WholeBodyReadResult)> steps);

}
