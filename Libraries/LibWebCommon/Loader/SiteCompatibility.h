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
#include <LibURL/Origin.h>
#include <LibURL/Site.h>
#include <LibWebCommon/Export.h>

namespace Web {

enum class UserAgentTransformation : u8 {
    HideLadybird,
    HideChromium,
};

class WEBCOMMON_API SiteCompatibilityRule {
public:
    static ErrorOr<SiteCompatibilityRule> from_json(JsonValue const&);
    static ErrorOr<SiteCompatibilityRule> create(Vector<String> sites, Vector<UserAgentTransformation> transformations, Vector<String> exposed_interfaces = {});

    Vector<URL::Site> const& sites() const { return m_sites; }
    Vector<UserAgentTransformation> const& user_agent_transformations() const { return m_user_agent_transformations; }
    Vector<String> const& exposed_interfaces() const { return m_exposed_interfaces; }

    bool matches(URL::Site const&) const;
    String apply_user_agent_transformations(StringView user_agent) const;
    bool exposes_experimental_interface(StringView name) const;

private:
    SiteCompatibilityRule(Vector<URL::Site> sites, Vector<UserAgentTransformation> transformations, Vector<String> exposed_interfaces);

    Vector<URL::Site> m_sites;
    Vector<UserAgentTransformation> m_user_agent_transformations;
    Vector<String> m_exposed_interfaces;
};

class WEBCOMMON_API SiteCompatibilityData {
public:
    static ErrorOr<SiteCompatibilityData> from_json(JsonValue const&);

    void add_rule(SiteCompatibilityRule rule) { m_rules.append(move(rule)); }

    Vector<SiteCompatibilityRule> const& rules() const { return m_rules; }
    // A rule applies to a document or worker when it matches either its top-level site or its own site.
    String user_agent_for(URL::Origin const& top_level_origin, URL::Origin const& origin, StringView default_user_agent) const;
    bool exposes_experimental_interface(URL::Origin const& top_level_origin, URL::Origin const& origin, StringView name) const;

private:
    Vector<SiteCompatibilityRule> m_rules;
};

}
