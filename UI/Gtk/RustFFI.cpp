/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibWebView/Application.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/URL.h>
#include <UI/Gtk/RustFFI.h>

// C++ side of the C++ <-> Rust FFI boundary. As more UI components move to
// Rust, the helpers in here will grow and shrink to match the boundary.

namespace Ladybird::FFI {

namespace {

struct LadybirdAutocomplete {
    WebView::Autocomplete autocomplete;
    void* user_data { nullptr };
    void (*destroy)(void*) { nullptr };

    ~LadybirdAutocomplete()
    {
        if (destroy)
            destroy(user_data);
    }
};

}

extern "C" uint8_t* ladybird_location_entry_placeholder_alloc(size_t* out_len)
{
    ByteString text;
    if (auto const& search_engine = WebView::Application::settings().search_engine(); search_engine.has_value())
        text = ByteString::formatted("Search with {} or enter URL", search_engine->name);
    else
        text = "Enter URL or search...";

    auto* buffer = static_cast<uint8_t*>(malloc(text.length()));
    if (!buffer) {
        *out_len = 0;
        return nullptr;
    }
    memcpy(buffer, text.characters(), text.length());
    *out_len = text.length();
    return buffer;
}

extern "C" uint8_t* ladybird_location_entry_sanitize_url_alloc(uint8_t const* input, size_t input_len, size_t* out_len)
{
    auto query = StringView(reinterpret_cast<char const*>(input), input_len);
    auto url = WebView::sanitize_url(query, WebView::Application::settings().search_engine());
    if (!url.has_value()) {
        *out_len = 0;
        return nullptr;
    }

    auto serialized = url->serialize().to_byte_string();
    auto* buffer = static_cast<uint8_t*>(malloc(serialized.length()));
    if (!buffer) {
        *out_len = 0;
        return nullptr;
    }
    memcpy(buffer, serialized.characters(), serialized.length());
    *out_len = serialized.length();
    return buffer;
}

extern "C" bool ladybird_location_entry_break_url_into_parts(
    uint8_t const* input,
    size_t input_len,
    size_t* scheme_and_subdomain_len,
    size_t* effective_tld_plus_one_len)
{
    auto url_str = StringView(reinterpret_cast<char const*>(input), input_len);
    auto parts = WebView::break_url_into_parts(url_str);
    if (!parts.has_value())
        return false;
    *scheme_and_subdomain_len = parts->scheme_and_subdomain.length();
    *effective_tld_plus_one_len = parts->effective_tld_plus_one.length();
    return true;
}

extern "C" void ladybird_string_free(uint8_t* p)
{
    free(p);
}

extern "C" void* ladybird_autocomplete_new(AutocompleteCallback callback, void* user_data, void (*destroy)(void*))
{
    auto wrapper = new LadybirdAutocomplete();
    wrapper->user_data = user_data;
    wrapper->destroy = destroy;

    wrapper->autocomplete.on_autocomplete_query_complete = [callback, ud = user_data](Vector<WebView::AutocompleteSuggestion> suggestions, WebView::AutocompleteResultKind) {
        Vector<ByteString> texts;
        texts.ensure_capacity(suggestions.size());
        Vector<uint8_t const*> ptrs;
        ptrs.ensure_capacity(suggestions.size());
        Vector<size_t> lens;
        lens.ensure_capacity(suggestions.size());
        for (auto const& s : suggestions) {
            texts.append(s.text.to_byte_string());
            auto const& bs = texts.last();
            ptrs.append(reinterpret_cast<uint8_t const*>(bs.characters()));
            lens.append(bs.length());
        }
        callback(ptrs.data(), lens.data(), suggestions.size(), ud);
    };

    return wrapper;
}

extern "C" void ladybird_autocomplete_free(void* p)
{
    delete static_cast<LadybirdAutocomplete*>(p);
}

extern "C" void ladybird_autocomplete_query(void* p, uint8_t const* query, size_t query_len)
{
    if (!p)
        return;
    auto* wrapper = static_cast<LadybirdAutocomplete*>(p);
    auto query_str = MUST(String::from_utf8(StringView(reinterpret_cast<char const*>(query), query_len)));
    wrapper->autocomplete.query_autocomplete_engine(query_str);
}

}
