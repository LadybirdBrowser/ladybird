/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>

namespace RequestServer {

// The browser profile whose TLS handshake and HTTP/2 preface libcurl sends for a User-Agent string: the one that
// names the browser the string claims to be. A string that claims no browser profile gets none, and libcurl's own
// handshake goes out.
Optional<StringView> impersonation_target_for_user_agent(StringView user_agent);

// Shapes the easy handle's TLS handshake and HTTP/2 preface like the browser the User-Agent string claims, when
// libcurl carries the browser profiles. The request's own headers stay as they are. Returns whether a profile was
// applied.
bool impersonate_user_agent(void* curl_easy_handle, StringView user_agent);

}
