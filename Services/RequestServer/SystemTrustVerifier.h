/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/URL.h>

namespace RequestServer {

struct TLSVerificationContext {
    URL::URL url;
};

int setup_system_trust_verifier(void*, void* ssl_ctx, void*);

}
