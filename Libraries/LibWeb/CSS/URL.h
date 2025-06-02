/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibWeb/CSS/Enums.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-values-5/#request-url-modifiers
class RequestURLModifier {
public:
    enum class Type : u8 {
        CrossOrigin,
        Integrity,
        ReferrerPolicy,
    };

    static RequestURLModifier create_cross_origin(CrossOriginModifierValue);
    static RequestURLModifier create_integrity(FlyString);
    static RequestURLModifier create_referrer_policy(ReferrerPolicyModifierValue);

    ~RequestURLModifier() = default;
    void modify_request(GC::Ref<Fetch::Infrastructure::Request>) const;
    Type type() const { return m_type; }
    String to_string() const;
    bool operator==(RequestURLModifier const&) const;

private:
    using Value = Variant<CrossOriginModifierValue, ReferrerPolicyModifierValue, FlyString>;
    RequestURLModifier(Type, Value);

    Type m_type;
    Value m_value;
};

// https://drafts.csswg.org/css-values-4/#urls
class URL {
public:
    URL(String url, Vector<RequestURLModifier> = {});

    String const& url() const { return m_url; }
    Vector<RequestURLModifier> const& request_url_modifiers() const { return m_request_url_modifiers; }

    String to_string() const;
    bool operator==(URL const&) const;

private:
    String m_url;
    Vector<RequestURLModifier> m_request_url_modifiers;
};

}

template<>
struct AK::Formatter<Web::CSS::URL> : AK::Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::URL const& value)
    {
        return Formatter<StringView>::format(builder, value.to_string());
    }
};
