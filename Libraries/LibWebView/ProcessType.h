/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace WebView {

enum class ProcessType : u8 {
    Browser,
    WebContent,
    WebWorker,
    RequestServer,
    ImageDecoder,
};

}
