/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Fuzzing/BoundedInput.h"
#include <AK/Array.h>
#include <AK/WeakPtr.h>
#include <LibCore/EventLoop.h>
#include <LibHTTP/Cookie/ParsedCookie.h>
#include <LibURL/Parser.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/StorageJar.h>

extern "C" int LLVMFuzzerTestOneInput(u8 const* data, size_t size)
{
    AK::set_debug_enabled(false);
    if (size > 4096)
        return 0;
    Core::EventLoop loop;
    using namespace WebView;
    Array<RefPtr<BrowsingSession>, 2> sessions { BrowsingSession::create(IsPrivate::No), BrowsingSession::create(IsPrivate::Yes) };
    // The ordinary factory intentionally leaves startup-owned stores empty.
    // Use transient stores here: disk persistence is a separate browser lane.
    sessions[0]->cookie_jar = CookieJar::create(IsPrivate::No);
    sessions[0]->storage_jar = StorageJar::create();
    Array<URL::URL, 2> urls { URL::Parser::basic_parse("https://a.invalid/"sv).release_value(), URL::Parser::basic_parse("https://b.invalid/"sv).release_value() };
    Array<String, 2> keys { "https://a.invalid"_string, "https://b.invalid"_string };
    Array<StorageEndpointType, 2> endpoints { StorageEndpointType::LocalStorage, StorageEndpointType::SessionStorage };
    Array<Array<Array<Optional<Utf16String>, 2>, 2>, 2> values;
    Array<Array<Optional<String>, 2>, 2> cookies;
    Array<Array<bool, 2>, 2> http_only {};
    Fuzzing::BoundedInput input({ data, size });
    for (size_t step = 0; step < 64 && (step < 2 || !input.remaining().is_empty()); ++step) {
        size_t owner = step < 2 ? step : input.byte() % 2;
        size_t site = input.byte() % 2;
        size_t kind = input.byte() % 2;
        auto operation = step < 2 ? 0 : input.byte() % 7;
        auto& session = *sessions[owner];
        switch (operation) {
        case 0: {
            auto value = Utf16String::number(input.byte());
            auto result = session.storage_jar->set_item(endpoints[kind], keys[site], u"sentinel"_utf16, value);
            VERIFY(result.has<Optional<Utf16String>>());
            VERIFY(result.get<Optional<Utf16String>>() == values[owner][site][kind]);
            values[owner][site][kind] = move(value);
            break;
        }
        case 1:
            session.storage_jar->remove_item(endpoints[kind], keys[site], u"sentinel"_utf16);
            values[owner][site][kind].clear();
            break;
        case 2:
            session.storage_jar->clear_storage_key(endpoints[kind], keys[site]);
            values[owner][site][kind].clear();
            break;
        case 3: {
            auto value = MUST(String::formatted("v{}", input.byte()));
            bool hidden = input.byte() & 1;
            auto parsed = HTTP::Cookie::parse_cookie(urls[site], MUST(String::formatted("sid={}; Path=/{}", value, hidden ? "; HttpOnly"sv : ""sv)));
            VERIFY(parsed.has_value());
            session.cookie_jar->set_cookie(urls[site], *parsed, HTTP::Cookie::Source::Http);
            cookies[owner][site] = move(value);
            http_only[owner][site] = hidden;
            break;
        }
        case 4:
            session.cookie_jar->delete_all_cookies(urls[site]);
            cookies[owner][site].clear();
            break;
        case 5: {
            auto weak = sessions[1]->make_weak_ptr();
            sessions[1] = nullptr;
            VERIFY(!weak);
            sessions[1] = BrowsingSession::create(IsPrivate::Yes);
            values[1] = {};
            cookies[1] = {};
            http_only[1] = {};
            break;
        }
        case 6:
            break; // Read-only control: verify every session/site below.
        }
        for (size_t observed_owner = 0; observed_owner < 2; ++observed_owner) {
            for (size_t observed_site = 0; observed_site < 2; ++observed_site) {
                auto& observed = *sessions[observed_owner];
                for (size_t observed_kind = 0; observed_kind < 2; ++observed_kind) {
                    VERIFY(observed.storage_jar->get_item(endpoints[observed_kind], keys[observed_site], u"sentinel"_utf16) == values[observed_owner][observed_site][observed_kind]);
                }
                auto expected = cookies[observed_owner][observed_site].has_value()
                    ? MUST(String::formatted("sid={}", *cookies[observed_owner][observed_site]))
                    : String {};
                VERIFY(observed.cookie_jar->get_cookie(urls[observed_site], HTTP::Cookie::Source::Http) == expected);
                if (http_only[observed_owner][observed_site])
                    expected = {};
                VERIFY(observed.cookie_jar->get_cookie(urls[observed_site], HTTP::Cookie::Source::NonHttp) == expected);
            }
        }
    }
    return 0;
}
