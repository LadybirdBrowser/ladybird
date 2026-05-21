/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Forward.h>

namespace Web {

class AsciiStringMatcher {
public:
    explicit AsciiStringMatcher(ReadonlySpan<String> patterns);

    bool contains(StringView text) const;

private:
    struct Transition {
        u8 character { 0 };
        u32 next_state { 0 };
    };

    struct Node {
        u32 first_transition { 0 };
        u8 transition_count { 0 };
        bool output { false };
    };

    Vector<Node> m_nodes;
    Vector<Transition> m_transitions;
};

class WEB_API ContentBlocker {
    AK_MAKE_NONCOPYABLE(ContentBlocker);

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

    bool filtering_enabled() const { return m_filtering_enabled; }
    void set_filtering_enabled(bool const enabled) { m_filtering_enabled = enabled; }

    bool is_filtered(URL::URL const&) const;
    bool is_filtered(URL::URL const&, URL::URL const& source_url, ResourceType) const;
    bool is_filtered(URL::URL const&, URL::URL const& source_url, Optional<Fetch::Infrastructure::Request::Destination> const&, Optional<Fetch::Infrastructure::Request::InitiatorType> const&, Fetch::Infrastructure::Request::Mode) const;
    ErrorOr<void> set_patterns(ReadonlySpan<String>);

    bool has_cosmetic_rules() const { return !m_cosmetic_rules.is_empty(); }
    String cosmetic_style_sheet_for_document(DOM::Document const&) const;
    String cosmetic_style_sheet_for_url(URL::URL const&) const;

    static ResourceType resource_type_from_fetch_metadata(Optional<Fetch::Infrastructure::Request::Destination> const&, Optional<Fetch::Infrastructure::Request::InitiatorType> const&, Fetch::Infrastructure::Request::Mode);
    static URL::URL source_url_for_matching(URL::URL const&);

private:
    ContentBlocker();
    ~ContentBlocker();

    bool contains(StringView text) const;

    struct CosmeticRule {
        Optional<String> domain;
        String selector;
    };

    bool m_filtering_enabled { true };
    OwnPtr<AsciiStringMatcher> m_matcher;
    Vector<CosmeticRule> m_cosmetic_rules;
};

}
