/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/JsonValue.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibURL/RustIntegration.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>

namespace Web {

enum class UserAgentTransformation : u8 {
    HideLadybird,
};

class WEB_API SiteCompatibilityRule {
public:
    static ErrorOr<SiteCompatibilityRule> from_json(JsonValue const&);
    static ErrorOr<SiteCompatibilityRule> create(Vector<String> patterns, Vector<UserAgentTransformation> transformations);

    Vector<String> const& patterns() const { return m_patterns; }
    Vector<UserAgentTransformation> const& user_agent_transformations() const { return m_user_agent_transformations; }

    bool matches(URL::URL const&) const;
    String apply_user_agent_transformations(StringView user_agent) const;

private:
    SiteCompatibilityRule(Vector<String> patterns, Vector<URL::RustIntegration::URLPattern> compiled_patterns, Vector<UserAgentTransformation> transformations);

    Vector<String> m_patterns;
    Vector<URL::RustIntegration::URLPattern> m_compiled_patterns;
    Vector<UserAgentTransformation> m_user_agent_transformations;
};

class WEB_API SiteCompatibilityData {
public:
    static ErrorOr<SiteCompatibilityData> from_json(JsonValue const&);

    void add_rule(SiteCompatibilityRule rule) { m_rules.append(move(rule)); }

    Vector<SiteCompatibilityRule> const& rules() const { return m_rules; }
    String user_agent_for_url(URL::URL const&, StringView default_user_agent) const;
    String user_agent_for_websocket_url(URL::URL const&, StringView default_user_agent) const;

private:
    Vector<SiteCompatibilityRule> m_rules;
};

}
