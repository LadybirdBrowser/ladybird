/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <AK/String.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

namespace Web {

class WEB_API ContentBlocker {
    AK_MAKE_NONCOPYABLE(ContentBlocker);
    AK_MAKE_NONMOVABLE(ContentBlocker);

public:
    enum class ResourceType : u8 {
        Document,
        Font,
        Image,
        Media,
        Object,
        Other,
        Ping,
        Script,
        Stylesheet,
        Subdocument,
        WebSocket,
        XMLHttpRequest,
    };

    static ContentBlocker& the();

    bool has_rules() const { return m_engine != nullptr; }
    bool has_cosmetic_rules() const { return m_has_cosmetic_rules; }
    bool filtering_enabled() const { return m_filtering_enabled; }
    void set_filtering_enabled(bool const enabled) { m_filtering_enabled = enabled; }

    bool is_filtered(URL::URL const&) const;
    bool is_filtered(URL::URL const&, URL::URL const& source_url, ResourceType) const;
    bool is_filtered(URL::URL const&, URL::URL const& source_url, Optional<Fetch::Infrastructure::Request::Destination> const&, Optional<Fetch::Infrastructure::Request::InitiatorType> const&, Fetch::Infrastructure::Request::Mode) const;
    ErrorOr<void> set_patterns(ReadonlySpan<String>);
    ErrorOr<void> set_rules_from_bytes(ReadonlyBytes);

    String cosmetic_style_sheet_for_url(URL::URL const&) const;
    String cosmetic_style_sheet_for_url(URL::URL const&, ReadonlySpan<String> classes, ReadonlySpan<String> ids) const;
    bool has_generic_cosmetic_selectors_for_url(URL::URL const&, ReadonlySpan<String> classes, ReadonlySpan<String> ids) const;

    static ResourceType resource_type_from_fetch_metadata(Optional<Fetch::Infrastructure::Request::Destination> const&, Optional<Fetch::Infrastructure::Request::InitiatorType> const&, Fetch::Infrastructure::Request::Mode);
    static URL::URL source_url_for_matching(URL::URL const&);

private:
    ContentBlocker();
    ~ContentBlocker();

    bool m_filtering_enabled { true };
    bool m_has_cosmetic_rules { false };
    void* m_engine { nullptr };
};

}
