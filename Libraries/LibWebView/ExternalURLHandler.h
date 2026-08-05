/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <LibURL/URL.h>
#include <LibWebView/Export.h>

namespace WebView {

class WEBVIEW_API ExternalURLHandler : public RefCounted<ExternalURLHandler> {
public:
    using LaunchCallback = Function<void(bool)>;

    virtual ~ExternalURLHandler() = default;

    String const& application_name() const { return m_application_name; }
    bool is_ladybird() const { return m_is_ladybird; }

    virtual void launch(URL::URL const&, LaunchCallback) const = 0;

protected:
    ExternalURLHandler(String application_name, bool is_ladybird)
        : m_application_name(move(application_name))
        , m_is_ladybird(is_ladybird)
    {
    }

private:
    String m_application_name;
    bool m_is_ladybird { false };
};

using ExternalURLHandlerCallback = Function<void(RefPtr<ExternalURLHandler>)>;

enum class ExternalURLAction : u8 {
    Block,
    Launch,
    Prompt,
};

enum class ExternalURLRequestSource : u8 {
    Page,
    BrowserUI,
};

class WEBVIEW_API ExternalURLRequestPolicy {
public:
    ExternalURLAction decide(URL::URL const&, bool has_transient_activation, MonotonicTime now = MonotonicTime::now());
    void allow_next_request(MonotonicTime now = MonotonicTime::now()) { m_request_allowance = RequestAllowance { ExternalURLRequestSource::Page, {}, now }; }
    void allow_request_from_browser_ui(URL::URL const& url, MonotonicTime now = MonotonicTime::now()) { m_request_allowance = RequestAllowance { ExternalURLRequestSource::BrowserUI, url, now }; }
    Optional<ExternalURLRequestSource> take_request_allowance();
    void clear_page_request_allowance();

    static bool is_denied_scheme(StringView);

private:
    struct RequestAllowance {
        ExternalURLRequestSource source;
        Optional<URL::URL> expected_url;
        MonotonicTime granted_at;
    };

    Optional<RequestAllowance> m_request_allowance;
};

}
