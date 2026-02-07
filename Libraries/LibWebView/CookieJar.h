/*
 * Copyright (c) 2021-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Traits.h>
#include <LibCore/Timer.h>
#include <LibDatabase/Forward.h>
#include <LibHTTP/Cookie/Cookie.h>
#include <LibRequests/CacheSizes.h>
#include <LibURL/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct CookieStorageKey {
    bool operator==(CookieStorageKey const&) const = default;

    String name;
    String domain;
    String path;
};

class WEBVIEW_API CookieJar {
public:
    static ErrorOr<NonnullOwnPtr<CookieJar>> create(Database::Database&);
    static NonnullOwnPtr<CookieJar> create();

    ~CookieJar();

    String get_cookie(URL::URL const& url, HTTP::Cookie::Source source);
    void set_cookie(URL::URL const& url, HTTP::Cookie::ParsedCookie const& parsed_cookie, HTTP::Cookie::Source source);
    void update_cookie(HTTP::Cookie::Cookie);
    void dump_cookies();
    Vector<HTTP::Cookie::Cookie> get_all_cookies();
    Vector<HTTP::Cookie::Cookie> get_all_cookies_webdriver(URL::URL const& url);
    Vector<HTTP::Cookie::Cookie> get_all_cookies_cookiestore(URL::URL const& url);
    Optional<HTTP::Cookie::Cookie> get_named_cookie(URL::URL const& url, StringView name);
    void expire_cookies_with_time_offset(AK::Duration);
    void expire_cookies_accessed_since(UnixDateTime since);
    Requests::CacheSizes estimate_storage_size_accessed_since(UnixDateTime since) const;

private:
    struct Statements {
        Database::StatementID insert_cookie { 0 };
        Database::StatementID expire_cookie { 0 };
        Database::StatementID select_all_cookies { 0 };
    };

    class WEBVIEW_API TransientStorage {
    public:
        using Cookies = HashMap<CookieStorageKey, HTTP::Cookie::Cookie>;

        void set_cookies(Cookies);
        void set_cookie(CookieStorageKey, HTTP::Cookie::Cookie);
        Optional<HTTP::Cookie::Cookie const&> get_cookie(CookieStorageKey const&);

        size_t size() const { return m_cookies.size(); }

        UnixDateTime purge_expired_cookies(Optional<AK::Duration> offset = {});
        void expire_and_purge_cookies_accessed_since(UnixDateTime since);

        Requests::CacheSizes estimate_storage_size_accessed_since(UnixDateTime since) const;

        auto take_dirty_cookies() { return move(m_dirty_cookies); }

        template<typename Callback>
        void for_each_cookie(Callback callback)
        {
            using ReturnType = InvokeResult<Callback, HTTP::Cookie::Cookie&>;

            for (auto& it : m_cookies) {
                if constexpr (IsSame<ReturnType, IterationDecision>) {
                    if (callback(it.value) == IterationDecision::Break)
                        return;
                } else {
                    static_assert(IsSame<ReturnType, void>);
                    callback(it.value);
                }
            }
        }

    private:
        using CookieEntry = decltype(declval<Cookies>().take_all_matching(nullptr))::ValueType;
        static void send_cookie_changed_notifications(ReadonlySpan<CookieEntry>, bool inform_web_view_about_changed_domains = true);

        Cookies m_cookies;
        Cookies m_dirty_cookies;
    };

    struct WEBVIEW_API PersistedStorage {
        void insert_cookie(HTTP::Cookie::Cookie const& cookie);
        TransientStorage::Cookies select_all_cookies();

        Database::Database& database;
        Statements statements;
        RefPtr<Core::Timer> synchronization_timer {};
    };

    explicit CookieJar(Optional<PersistedStorage>);

    AK_MAKE_NONCOPYABLE(CookieJar);
    AK_MAKE_NONMOVABLE(CookieJar);

    enum class MatchingCookiesSpecMode {
        RFC6265,
        WebDriver,
    };

    Vector<HTTP::Cookie::Cookie> get_matching_cookies(URL::URL const& url, HTTP::Cookie::Source source, MatchingCookiesSpecMode mode = MatchingCookiesSpecMode::RFC6265);

    Optional<PersistedStorage> m_persisted_storage;
    TransientStorage m_transient_storage;
};

}

template<>
struct AK::Traits<WebView::CookieStorageKey> : public AK::DefaultTraits<WebView::CookieStorageKey> {
    static unsigned hash(WebView::CookieStorageKey const& key)
    {
        unsigned hash = 0;
        hash = pair_int_hash(hash, key.name.hash());
        hash = pair_int_hash(hash, key.domain.hash());
        hash = pair_int_hash(hash, key.path.hash());
        return hash;
    }
};
