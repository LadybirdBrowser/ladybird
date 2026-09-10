/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/Types.h>
#include <AK/Weakable.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

enum class IsPrivate : u8 {
    No,
    Yes,
};

// The ordinary session lives as long as the application. A private one is owned by its views and content processes,
// and everything else holds it weakly, so nothing outliving them can bring one back.
class WEBVIEW_API BrowsingSession
    : public RefCounted<BrowsingSession>
    , public Weakable<BrowsingSession> {
public:
    static NonnullRefPtr<BrowsingSession> create(IsPrivate);
    ~BrowsingSession();

    IsPrivate is_private() const { return m_is_private; }

    // NB: Null for the ordinary session until Application fills these in during startup.
    OwnPtr<CookieJar> cookie_jar;
    OwnPtr<StorageJar> storage_jar;
    OwnPtr<BlobURLStore> blob_url_store;
    OwnPtr<HSTSStore> hsts_store;
    OwnPtr<FaviconStore> favicon_store;
    OwnPtr<HistoryStore> history_store;
    OwnPtr<SessionStore> session_store;

private:
    explicit BrowsingSession(IsPrivate);

    IsPrivate m_is_private { IsPrivate::No };
};

}
