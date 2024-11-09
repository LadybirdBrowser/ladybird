/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
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
#include <LibCore/DateTime.h>
#include <LibCore/Timer.h>
#include <LibURL/Forward.h>
#include <LibWeb/Cookie/Cookie.h>
#include <LibWeb/Forward.h>
#include <LibWebView/Database.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct CookieStorageKey {
    bool operator==(CookieStorageKey const&) const = default;

    String name;
    String domain;
    String path;
};

class CookieJar {
    struct Statements {
        Database::StatementID insert_cookie { 0 };
        Database::StatementID expire_cookie { 0 };
        Database::StatementID select_all_cookies { 0 };
    };

    class TransientStorage {
    public:
        using Cookies = HashMap<CookieStorageKey, Web::Cookie::Cookie>;

        void set_cookies(Cookies);
        void set_cookie(CookieStorageKey, Web::Cookie::Cookie);
        Optional<Web::Cookie::Cookie> get_cookie(CookieStorageKey const&);

        size_t size() const { return m_cookies.size(); }

        UnixDateTime purge_expired_cookies(Optional<AK::Duration> offset = {});

        auto take_dirty_cookies() { return move(m_dirty_cookies); }

        template<typename Callback>
        void for_each_cookie(Callback callback)
        {
            using ReturnType = InvokeResult<Callback, Web::Cookie::Cookie&>;

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
        Cookies m_cookies;
        Cookies m_dirty_cookies;
    };

    struct PersistedStorage {
        void insert_cookie(Web::Cookie::Cookie const& cookie);
        TransientStorage::Cookies select_all_cookies();

        Database& database;
        Statements statements;
        RefPtr<Core::Timer> synchronization_timer {};
    };

public:
    static ErrorOr<NonnullOwnPtr<CookieJar>> create(Database&);
    static NonnullOwnPtr<CookieJar> create();

    ~CookieJar();

    String get_cookie(const URL::URL& url, Web::Cookie::Source source);
    void set_cookie(const URL::URL& url, Web::Cookie::ParsedCookie const& parsed_cookie, Web::Cookie::Source source);
    void update_cookie(Web::Cookie::Cookie);
    void dump_cookies();
    Vector<Web::Cookie::Cookie> get_all_cookies();
    Vector<Web::Cookie::Cookie> get_all_cookies(URL::URL const& url);
    Optional<Web::Cookie::Cookie> get_named_cookie(URL::URL const& url, StringView name);
    void expire_cookies_with_time_offset(AK::Duration);

private:
    explicit CookieJar(Optional<PersistedStorage>);

    AK_MAKE_NONCOPYABLE(CookieJar);
    AK_MAKE_NONMOVABLE(CookieJar);

    static Optional<String> canonicalize_domain(const URL::URL& url);
    static bool path_matches(StringView request_path, StringView cookie_path);

    enum class MatchingCookiesSpecMode {
        RFC6265,
        WebDriver,
    };

    void store_cookie(Web::Cookie::ParsedCookie const& parsed_cookie, const URL::URL& url, String canonicalized_domain, Web::Cookie::Source source);
    Vector<Web::Cookie::Cookie> get_matching_cookies(const URL::URL& url, StringView canonicalized_domain, Web::Cookie::Source source, MatchingCookiesSpecMode mode = MatchingCookiesSpecMode::RFC6265);

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
