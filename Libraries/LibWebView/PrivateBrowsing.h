/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/Types.h>
#include <LibWebView/Forward.h>

namespace WebView {

enum class IsPrivate : u8 {
    No,
    Yes,
};

struct PrivateBrowsingSession {
    ~PrivateBrowsingSession();

    NonnullOwnPtr<CookieJar> cookie_jar;
    NonnullOwnPtr<StorageJar> storage_jar;
    NonnullOwnPtr<HSTSStore> hsts_store;
    NonnullOwnPtr<HistoryStore> history_store;
};

}
