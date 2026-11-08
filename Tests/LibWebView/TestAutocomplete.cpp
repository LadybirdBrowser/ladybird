/*
 * Copyright (c) 2026, sideshowbarker <mike@w3.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/String.h>
#include <LibCore/EventLoop.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Application.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/AutocompleteRanker.h>
#include <LibWebView/AutocompleteService.h>
#include <LibWebView/Settings.h>
#include <stdlib.h>

namespace {

class TestExternalURLHandler final : public WebView::ExternalURLHandler {
public:
    TestExternalURLHandler()
        : WebView::ExternalURLHandler("Test application"_string, false)
    {
    }

    virtual void launch(URL::URL const&, LaunchCallback callback) const override
    {
        callback(true);
    }
};

class TestApplication
    : public Test::HeadlessApplication
    , public WebView::AutocompleteHost {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::Yes;
    }

    virtual void resolve_external_url_handler(URL::URL const& url, WebView::ExternalURLHandlerCallback callback) const override
    {
        m_pending_external_url_handler_queries.append({ url, move(callback) });
    }

    virtual WebView::AutocompleteService& autocomplete_service() override
    {
        if (!m_autocomplete_service)
            m_autocomplete_service = make<WebView::AutocompleteService>(Core::EventLoop::current(), Optional<ByteString> {});
        return *m_autocomplete_service;
    }

    virtual WebView::Settings& autocomplete_settings() override { return WebView::Application::settings(); }

    virtual Function<void()> fetch_remote_suggestions(WebView::SearchEngine const& engine, String const& query, WebView::IsPrivate is_private, Function<void(RemoteResponse)> on_response) override
    {
        auto url = engine.suggestions_url_for_query(query);
        auto client = WebView::Application::request_server_client(is_private);
        if (!url.has_value() || client.is_error()) {
            on_response({ .had_network_error = true, .response_code = {}, .content_type = {}, .body = {} });
            return [] { };
        }
        auto request = client.value()->start_request("GET"sv, *url, HTTP::Cookie::SameSiteContext::same_site());
        auto request_id = m_next_request_id++;
        request->set_buffered_request_finished_callback(
            [this, request_id, on_response = move(on_response)](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> const& network_error,
                HTTP::HeaderList const& headers, Optional<u32> response_code, Optional<String> const&, Requests::CacheState, Core::ImmutableBytes payload) {
                // A canceled request can still finish before its deferred stop runs.
                if (!m_requests.contains(request_id))
                    return;

                // A request cannot be destroyed from inside its own callback.
                Core::deferred_invoke([this, request_id] { m_requests.remove(request_id); });
                Optional<ByteString> content_type;
                if (auto value = headers.get("Content-Type"sv); value.has_value())
                    content_type = *value;
                on_response({
                    .had_network_error = network_error.has_value(),
                    .response_code = response_code,
                    .content_type = move(content_type),
                    .body = MUST(ByteBuffer::copy(payload.bytes())),
                });
            });
        m_requests.set(request_id, request.release_nonnull());
        return [this, request_id] {
            if (auto request = m_requests.get(request_id); request.has_value()) {
                Core::deferred_invoke([request = *request] { request->stop(); });
                m_requests.remove(request_id);
            }
        };
    }

    size_t pending_external_url_handler_query_count() const { return m_pending_external_url_handler_queries.size(); }

    StringView pending_external_url_handler_query_scheme(size_t index) const
    {
        return m_pending_external_url_handler_queries[index].url.scheme();
    }

    void complete_next_external_url_handler_query(bool has_handler)
    {
        auto query = m_pending_external_url_handler_queries.take(0);
        RefPtr<WebView::ExternalURLHandler> handler;
        if (has_handler)
            handler = adopt_ref(*new TestExternalURLHandler);
        query.callback(move(handler));
    }

private:
    struct PendingExternalURLHandlerQuery {
        URL::URL url;
        WebView::ExternalURLHandlerCallback callback;
    };

    mutable Vector<PendingExternalURLHandlerQuery> m_pending_external_url_handler_queries;
    OwnPtr<WebView::AutocompleteService> m_autocomplete_service;
    HashMap<u64, NonnullRefPtr<Requests::Request>> m_requests;
    u64 m_next_request_id { 1 };
};

}

// Canceling the pending autocomplete query from inside the query-complete callback is exactly what the Qt location bar
// does: Activating a suggestion clears the bar's focus — whose focusOutEvent calls Autocomplete::cancel_pending_query.
// That cancel runs while the request's completion callback is still on the stack — so, tearing the request down freed a
// callback mid-call, and tripped an assert. Regression test for #10278.

TEST_CASE(autocomplete)
{
    Test::HeadlessTestEnvironment environment { "TestAutocomplete"sv };

    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());

    WebView::Application::settings().set_search_engine_settings({});
    auto saw_bookmark = false;
    auto bookmark_query_completed = false;
    {
        WebView::Autocomplete autocomplete { *app, WebView::IsPrivate::No };
        autocomplete.on_autocomplete_query_complete = [&](auto, auto const& suggestions, WebView::AutocompleteResultKind kind) {
            VERIFY(kind == WebView::AutocompleteResultKind::Final);
            saw_bookmark |= suggestions.contains([](auto const& suggestion) {
                return suggestion.text == "https://ladybird.test/"sv;
            });
            autocomplete.cancel_pending_query();
            bookmark_query_completed = true;
        };

        Vector<WebView::AutocompleteBookmark> bookmarks;
        bookmarks.append({ .url = "https://ladybird.test/"_string, .title = "Ladybird test bookmark"_string, .folder = {}, .favicon_png = {} });
        autocomplete.query_autocomplete_engine(1, "lady"_string, WebView::default_autocomplete_suggestion_limit, WebView::rank_bookmark_suggestions("lady"sv, bookmarks, 32));
        Core::EventLoop::current().spin_until([&]() { return bookmark_query_completed; });
    }

    VERIFY(saw_bookmark);

    auto saw_adaptive_result = false;
    {
        WebView::Autocomplete autocomplete { *app, WebView::IsPrivate::No };
        autocomplete.record_engagement({
            .input = "docs"_string,
            .destination_kind = WebView::OmniboxDestinationKind::URL,
            .destination = "https://example.com/manual/"_string,
            .was_explicit = true,
        });
        autocomplete.on_autocomplete_query_complete = [&](auto, auto const& suggestions, WebView::AutocompleteResultKind kind) {
            if (kind != WebView::AutocompleteResultKind::Final)
                return;
            saw_adaptive_result = suggestions.contains([](auto const& suggestion) {
                return suggestion.source == WebView::AutocompleteSuggestionSource::Adaptive;
            });
            autocomplete.cancel_pending_query();
        };
        autocomplete.query_autocomplete_engine(2, "docs"_string);
        Core::EventLoop::current().spin_until([&]() { return saw_adaptive_result; });
    }

    auto private_read_completed = false;
    {
        WebView::Autocomplete private_autocomplete { *app, WebView::IsPrivate::Yes };
        private_autocomplete.on_autocomplete_query_complete = [&](auto, auto const& suggestions, WebView::AutocompleteResultKind kind) {
            if (kind != WebView::AutocompleteResultKind::Final)
                return;
            VERIFY(suggestions.contains([](auto const& suggestion) {
                return suggestion.source == WebView::AutocompleteSuggestionSource::Adaptive
                    && suggestion.text == "https://example.com/manual/"sv;
            }));
            private_autocomplete.cancel_pending_query();
            private_read_completed = true;
        };
        private_autocomplete.query_autocomplete_engine(3, "docs"_string);
        Core::EventLoop::current().spin_until([&]() { return private_read_completed; });
    }

    {
        WebView::Autocomplete private_autocomplete { *app, WebView::IsPrivate::Yes };
        private_autocomplete.record_engagement({
            .input = "privatealias"_string,
            .destination_kind = WebView::OmniboxDestinationKind::URL,
            .destination = "https://private.example/"_string,
            .was_explicit = true,
        });
    }

    auto private_query_completed = false;
    {
        WebView::Autocomplete autocomplete { *app, WebView::IsPrivate::No };
        autocomplete.on_autocomplete_query_complete = [&](auto, auto const& suggestions, WebView::AutocompleteResultKind kind) {
            if (kind != WebView::AutocompleteResultKind::Final)
                return;
            VERIFY(!suggestions.contains([](auto const& suggestion) {
                return suggestion.source == WebView::AutocompleteSuggestionSource::Adaptive;
            }));
            autocomplete.cancel_pending_query();
            private_query_completed = true;
        };
        autocomplete.query_autocomplete_engine(4, "privatealias"_string);
        Core::EventLoop::current().spin_until([&]() { return private_query_completed; });
    }

    auto short_bookmark_query_completed = false;
    {
        WebView::Autocomplete autocomplete { *app, WebView::IsPrivate::No };
        autocomplete.on_autocomplete_query_complete = [&](auto, auto const& suggestions, WebView::AutocompleteResultKind kind) {
            if (kind != WebView::AutocompleteResultKind::Final)
                return;
            // NB: Without a search engine, the offer to choose one comes first, where the search would be.
            VERIFY(suggestions.size() >= 2);
            VERIFY(suggestions[0].source == WebView::AutocompleteSuggestionSource::ChooseSearchEngine);
            auto const& bookmark = suggestions[1];
            VERIFY(bookmark.text == "https://github.com/LadybirdBrowser/ladybird"sv);
            VERIFY(bookmark.title == "GH"sv);
            VERIFY(bookmark.match_class == WebView::AutocompleteMatchClass::ExactTitle);
            VERIFY(!bookmark.can_be_automatically_selected);
            VERIFY(!bookmark.can_be_inline_completed);
            autocomplete.cancel_pending_query();
            short_bookmark_query_completed = true;
        };
        Vector<WebView::AutocompleteBookmark> bookmarks;
        bookmarks.append({ .url = "https://github.com/LadybirdBrowser/ladybird"_string, .title = "GH"_string, .folder = {}, .favicon_png = {} });
        autocomplete.query_autocomplete_engine(5, "GH"_string, WebView::default_autocomplete_suggestion_limit, WebView::rank_bookmark_suggestions("GH"sv, bookmarks, 32));
        Core::EventLoop::current().spin_until([&]() { return short_bookmark_query_completed; });
    }

    auto choose_search_engine_query_completed = false;
    {
        WebView::Autocomplete autocomplete { *app, WebView::IsPrivate::No };
        autocomplete.on_autocomplete_query_complete = [&](auto, auto const& suggestions, WebView::AutocompleteResultKind kind) {
            if (kind != WebView::AutocompleteResultKind::Final)
                return;
            VERIFY(!suggestions.is_empty());
            auto const& suggestion = suggestions.first();
            VERIFY(suggestion.source == WebView::AutocompleteSuggestionSource::ChooseSearchEngine);
            VERIFY(suggestion.text == "about:settings#search"sv);
            VERIFY(suggestion.subtitle == "to search for “ladybird beetle”"sv);
            VERIFY(suggestion.can_be_automatically_selected);
            VERIFY(WebView::autocomplete_suggestion_display_text(suggestion) == "ladybird beetle"sv);
            autocomplete.cancel_pending_query();
            choose_search_engine_query_completed = true;
        };
        autocomplete.query_autocomplete_engine(6, "ladybird beetle"_string);
        Core::EventLoop::current().spin_until([&]() { return choose_search_engine_query_completed; });
    }

    WebView::Application::settings().set_search_engine_settings({
        .engine = WebView::Application::settings().find_search_engine_by_name("Google"sv),
    });
    auto whitespace_query_completed = false;
    {
        WebView::Autocomplete autocomplete { *app, WebView::IsPrivate::No };
        autocomplete.on_autocomplete_query_complete = [&](auto, auto const& suggestions, WebView::AutocompleteResultKind kind) {
            if (kind != WebView::AutocompleteResultKind::Final)
                return;
            VERIFY(suggestions.contains([](auto const& suggestion) {
                return suggestion.is_verbatim
                    && suggestion.text == "  spaced   query  "sv;
            }));
            autocomplete.cancel_pending_query();
            whitespace_query_completed = true;
        };
        autocomplete.query_autocomplete_engine(6, "  spaced   query  "_string);
        Core::EventLoop::current().spin_until([&]() { return whitespace_query_completed; });
    }

    auto about_query_completed = false;
    {
        WebView::Autocomplete autocomplete { *app, WebView::IsPrivate::No };
        autocomplete.on_autocomplete_query_complete = [&](auto, auto const& suggestions, WebView::AutocompleteResultKind kind) {
            if (kind != WebView::AutocompleteResultKind::Final)
                return;
            VERIFY(!suggestions.contains([](auto const& suggestion) {
                return suggestion.source == WebView::AutocompleteSuggestionSource::Search;
            }));
            VERIFY(suggestions.contains([](auto const& suggestion) {
                return suggestion.source == WebView::AutocompleteSuggestionSource::WebUI
                    && suggestion.text == "about:settings"sv;
            }));
            autocomplete.cancel_pending_query();
            about_query_completed = true;
        };
        autocomplete.query_autocomplete_engine(100, "about:set"_string);
        Core::EventLoop::current().spin_until([&]() { return about_query_completed; });
    }

    auto expect_internal_url_suggestion = [&](WebView::AutocompleteQueryID query_id, StringView input) {
        auto query_completed = false;
        WebView::Autocomplete autocomplete { *app, WebView::IsPrivate::No };
        autocomplete.on_autocomplete_query_complete = [&](auto, auto const& suggestions, WebView::AutocompleteResultKind kind) {
            if (kind != WebView::AutocompleteResultKind::Final)
                return;
            VERIFY(!suggestions.is_empty());
            VERIFY(suggestions.first().source == WebView::AutocompleteSuggestionSource::LiteralURL);
            VERIFY(suggestions.first().text == input);
            autocomplete.cancel_pending_query();
            query_completed = true;
        };

        auto pending_handler_queries = app->pending_external_url_handler_query_count();
        autocomplete.query_autocomplete_engine(query_id, MUST(String::from_utf8(input)));
        VERIFY(app->pending_external_url_handler_query_count() == pending_handler_queries);
        Core::EventLoop::current().spin_until([&]() { return query_completed; });
    };

    expect_internal_url_suggestion(106, "localhost:8080"sv);
    expect_internal_url_suggestion(107, "example.com:8080"sv);

    auto telephone_url_query_completed = false;
    {
        WebView::Autocomplete autocomplete { *app, WebView::IsPrivate::No };
        autocomplete.on_autocomplete_query_complete = [&](auto, auto const& suggestions, WebView::AutocompleteResultKind kind) {
            if (kind != WebView::AutocompleteResultKind::Final)
                return;
            VERIFY(!suggestions.is_empty());
            VERIFY(suggestions.first().source == WebView::AutocompleteSuggestionSource::LiteralURL);
            VERIFY(suggestions.first().text == "tel:123"sv);
            autocomplete.cancel_pending_query();
            telephone_url_query_completed = true;
        };
        autocomplete.query_autocomplete_engine(108, "tel:123"_string);
        VERIFY(app->pending_external_url_handler_query_count() == 1);
        VERIFY(app->pending_external_url_handler_query_scheme(0) == "tel"sv);
        app->complete_next_external_url_handler_query(true);
        Core::EventLoop::current().spin_until([&]() { return telephone_url_query_completed; });
    }

    auto external_url_local_query_completed = false;
    auto external_url_query_completed = false;
    {
        WebView::Autocomplete autocomplete { *app, WebView::IsPrivate::No };
        autocomplete.record_engagement({
            .input = "geo:23.4,12.4"_string,
            .destination_kind = WebView::OmniboxDestinationKind::URL,
            .destination = "https://ladybird.org/"_string,
            .was_explicit = true,
        });
        autocomplete.on_autocomplete_query_complete = [&](auto, auto const& suggestions, WebView::AutocompleteResultKind kind) {
            if (kind == WebView::AutocompleteResultKind::Intermediate
                && suggestions.contains([](auto const& suggestion) {
                       return suggestion.source == WebView::AutocompleteSuggestionSource::Adaptive
                           && suggestion.text == "https://ladybird.org/"sv;
                   })) {
                VERIFY(!suggestions.contains([](auto const& suggestion) {
                    return suggestion.source == WebView::AutocompleteSuggestionSource::LiteralURL;
                }));
                external_url_local_query_completed = true;
                return;
            }
            if (kind != WebView::AutocompleteResultKind::Final)
                return;
            VERIFY(!suggestions.is_empty());
            VERIFY(suggestions.first().source == WebView::AutocompleteSuggestionSource::LiteralURL);
            VERIFY(suggestions.first().text == "geo:23.4,12.4"sv);
            VERIFY(suggestions.contains([](auto const& suggestion) {
                return suggestion.source == WebView::AutocompleteSuggestionSource::Search
                    && suggestion.text == "geo:23.4,12.4"sv;
            }));
            autocomplete.cancel_pending_query();
            external_url_query_completed = true;
        };
        autocomplete.query_autocomplete_engine(101, "geo:23.4,12.4"_string);
        VERIFY(app->pending_external_url_handler_query_count() == 1);
        VERIFY(app->pending_external_url_handler_query_scheme(0) == "geo"sv);
        Core::EventLoop::current().spin_until([&]() { return external_url_local_query_completed; });
        app->complete_next_external_url_handler_query(true);
        Core::EventLoop::current().spin_until([&]() { return external_url_query_completed; });
    }

    auto unavailable_external_url_query_completed = false;
    {
        WebView::Autocomplete autocomplete { *app, WebView::IsPrivate::No };
        autocomplete.on_autocomplete_query_complete = [&](auto, auto const& suggestions, WebView::AutocompleteResultKind kind) {
            if (kind != WebView::AutocompleteResultKind::Final)
                return;
            VERIFY(!suggestions.is_empty());
            VERIFY(suggestions.first().source == WebView::AutocompleteSuggestionSource::Search);
            VERIFY(suggestions.first().text == "unknown:value"sv);
            VERIFY(!suggestions.contains([](auto const& suggestion) {
                return suggestion.source == WebView::AutocompleteSuggestionSource::LiteralURL;
            }));
            autocomplete.cancel_pending_query();
            unavailable_external_url_query_completed = true;
        };
        autocomplete.query_autocomplete_engine(102, "unknown:value"_string);
        VERIFY(app->pending_external_url_handler_query_count() == 1);
        VERIFY(app->pending_external_url_handler_query_scheme(0) == "unknown"sv);
        app->complete_next_external_url_handler_query(false);
        Core::EventLoop::current().spin_until([&]() { return unavailable_external_url_query_completed; });
    }

    auto replacement_query_completed = false;
    {
        WebView::Autocomplete autocomplete { *app, WebView::IsPrivate::No };
        autocomplete.on_autocomplete_query_complete = [&](auto query_id, auto const&, WebView::AutocompleteResultKind kind) {
            VERIFY(query_id != 103);
            if (query_id == 104 && kind == WebView::AutocompleteResultKind::Final) {
                autocomplete.cancel_pending_query();
                replacement_query_completed = true;
            }
        };
        autocomplete.query_autocomplete_engine(103, "geo:23.4,12.4"_string);
        VERIFY(app->pending_external_url_handler_query_count() == 1);
        autocomplete.query_autocomplete_engine(104, "replacement query"_string);
        app->complete_next_external_url_handler_query(true);
        Core::EventLoop::current().spin_until([&]() { return replacement_query_completed; });
    }

    {
        WebView::Autocomplete autocomplete { *app, WebView::IsPrivate::No };
        autocomplete.query_autocomplete_engine(105, "geo:23.4,12.4"_string);
        VERIFY(app->pending_external_url_handler_query_count() == 1);
    }
    app->complete_next_external_url_handler_query(true);

    // A closed loopback port makes the request fail fast and deterministically, with no real network.
    // Its on_finish synchronously delivers the final result. A data: URL cannot be used because the request
    // server's DNS path requires a host.
    WebView::Application::settings().add_custom_search_engine(WebView::SearchEngine {
        .name = "Test"_string,
        .query_url = "http://127.0.0.1:47919/search?q=%s"_string,
        .suggestions = WebView::SearchSuggestions { "http://127.0.0.1:47919/suggest?q=%s"_string },
    });
    WebView::Application::settings().set_search_engine_settings({
        .engine = WebView::Application::settings().find_search_engine_by_name("Test"sv),
        .suggestions = true,
    });

    WebView::Autocomplete autocomplete { *app, WebView::IsPrivate::No };
    auto request_completed = false;
    auto query_returned = false;
    autocomplete.on_autocomplete_query_complete = [&](auto, auto const&, WebView::AutocompleteResultKind kind) {
        if (kind != WebView::AutocompleteResultKind::Final)
            return;
        VERIFY(query_returned);
        autocomplete.cancel_pending_query();
        request_completed = true;
    };

    autocomplete.query_autocomplete_engine(7, "test"_string);
    query_returned = true;
    Core::EventLoop::current().spin_until([&]() { return request_completed; });

    outln("PASS: cancel_pending_query during query completion did not crash");
}
