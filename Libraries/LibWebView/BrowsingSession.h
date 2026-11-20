/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <AK/Vector.h>
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

    // No two sessions of the application share an ID.
    u64 id() const { return m_id; }

    // NB: Null for the ordinary session until Application fills these in during startup.
    OwnPtr<CookieJar> cookie_jar;
    OwnPtr<StorageJar> storage_jar;
    OwnPtr<BlobURLStore> blob_url_store;
    OwnPtr<HSTSStore> hsts_store;
    OwnPtr<PermissionManager> permission_manager;

    struct CertificateException {
        ByteString host;
        u16 port { 0 };
        ByteString fingerprint;

        bool operator==(CertificateException const&) const = default;
    };

    // Keep recent rejections until their warnings arrive, so users can only trust certificates actually presented.
    struct RejectedCertificate {
        CertificateException certificate;
        MonotonicTime rejected_at;
    };
    HashMap<ByteString, Vector<RejectedCertificate>> rejected_self_signed_certificates;

    // Every RequestServer of the session accepts these, including those launched after the user trusted them.
    Vector<CertificateException> trusted_certificates;

private:
    explicit BrowsingSession(IsPrivate);

    u64 m_id { 0 };
    IsPrivate m_is_private { IsPrivate::No };
};

}
