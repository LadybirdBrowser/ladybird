/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/URL.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

namespace Web::CSS {

URL::URL(String url, Type type, Vector<RequestURLModifier> request_url_modifiers)
    : m_type(type)
    , m_url(move(url))
    , m_request_url_modifiers(move(request_url_modifiers))
{
}

URL::URL(Utf16View url, Type type, Vector<RequestURLModifier> request_url_modifiers)
    : URL(MUST(url.to_utf8()), type, move(request_url_modifiers))
{
}

// https://drafts.csswg.org/cssom-1/#serialize-a-url
String URL::to_string() const
{
    return to_utf16_string().to_utf8();
}

// https://drafts.csswg.org/cssom-1/#serialize-a-url
Utf16String URL::to_utf16_string() const
{
    Utf16StringBuilder builder;
    switch (m_type) {
    case Type::Url:
        builder.append_ascii("url("sv);
        break;
    case Type::Src:
        builder.append_ascii("src("sv);
        break;
    }

    auto url = Utf16String::from_utf8_without_validation(m_url);
    serialize_a_string(builder, url);

    for (auto const& modifier : m_request_url_modifiers) {
        builder.append_ascii(' ');
        builder.append(modifier.to_utf16_string());
    }

    builder.append_ascii(')');
    return builder.to_string();
}

bool URL::operator==(URL const&) const = default;

RequestURLModifier RequestURLModifier::create_cross_origin(CrossOriginModifierValue value)
{
    return RequestURLModifier { Type::CrossOrigin, value };
}

RequestURLModifier RequestURLModifier::create_integrity(Utf16FlyString value)
{
    return RequestURLModifier { Type::Integrity, move(value) };
}

RequestURLModifier RequestURLModifier::create_referrer_policy(ReferrerPolicyModifierValue value)
{
    return RequestURLModifier { Type::ReferrerPolicy, value };
}

RequestURLModifier::RequestURLModifier(Type type, Value value)
    : m_type(type)
    , m_value(move(value))
{
}

void RequestURLModifier::modify_request(GC::Ref<Fetch::Infrastructure::Request> request) const
{
    switch (m_type) {
    case Type::CrossOrigin: {
        // https://drafts.csswg.org/css-values-5/#typedef-request-url-modifier-crossorigin-modifier
        // The URL request modifier steps for this modifier given request req are:

        // 1. Set req’s mode to "cors".
        request->set_mode(Fetch::Infrastructure::Request::Mode::CORS);

        // 2. If the given value is use-credentials, set req’s credentials mode to "include".
        if (m_value == CrossOriginModifierValue::UseCredentials) {
            request->set_credentials_mode(Fetch::Infrastructure::Request::CredentialsMode::Include);
        }
        // 3. Otherwise, set req’s credentials mode to "same-origin".
        else {
            request->set_credentials_mode(Fetch::Infrastructure::Request::CredentialsMode::SameOrigin);
        }
        break;
    }
    case Type::Integrity: {
        // https://drafts.csswg.org/css-values-5/#typedef-request-url-modifier-integrity-modifier

        // The URL request modifier steps for this modifier given request req are to set request’s integrity metadata to
        // the given <string>.
        request->set_integrity_metadata(m_value.get<Utf16FlyString>().to_utf16_string());
        break;
    }
    case Type::ReferrerPolicy: {
        // https://drafts.csswg.org/css-values-5/#typedef-request-url-modifier-referrerpolicy-modifier
        // The URL request modifier steps for this modifier given request req are to set request’s referrer policy to the
        // ReferrerPolicy that matches the given value.
        auto referrer_policy = [](ReferrerPolicyModifierValue value) {
            switch (value) {
            case ReferrerPolicyModifierValue::NoReferrer:
                return ReferrerPolicy::ReferrerPolicy::NoReferrer;
            case ReferrerPolicyModifierValue::NoReferrerWhenDowngrade:
                return ReferrerPolicy::ReferrerPolicy::NoReferrerWhenDowngrade;
            case ReferrerPolicyModifierValue::SameOrigin:
                return ReferrerPolicy::ReferrerPolicy::SameOrigin;
            case ReferrerPolicyModifierValue::Origin:
                return ReferrerPolicy::ReferrerPolicy::Origin;
            case ReferrerPolicyModifierValue::StrictOrigin:
                return ReferrerPolicy::ReferrerPolicy::StrictOrigin;
            case ReferrerPolicyModifierValue::OriginWhenCrossOrigin:
                return ReferrerPolicy::ReferrerPolicy::OriginWhenCrossOrigin;
            case ReferrerPolicyModifierValue::StrictOriginWhenCrossOrigin:
                return ReferrerPolicy::ReferrerPolicy::StrictOriginWhenCrossOrigin;
            case ReferrerPolicyModifierValue::UnsafeUrl:
                return ReferrerPolicy::ReferrerPolicy::UnsafeURL;
            }
            VERIFY_NOT_REACHED();
        }(m_value.get<ReferrerPolicyModifierValue>());
        request->set_referrer_policy(referrer_policy);
        break;
    }
    }
}

String RequestURLModifier::to_string() const
{
    return to_utf16_string().to_utf8();
}

Utf16String RequestURLModifier::to_utf16_string() const
{
    Utf16StringBuilder builder;
    switch (m_type) {
    case Type::CrossOrigin:
        builder.append_ascii("cross-origin("sv);
        builder.append_ascii(CSS::to_string(m_value.get<CrossOriginModifierValue>()));
        break;
    case Type::Integrity:
        builder.append_ascii("integrity("sv);
        serialize_a_string(builder, m_value.get<Utf16FlyString>());
        break;
    case Type::ReferrerPolicy:
        builder.append_ascii("referrer-policy("sv);
        builder.append_ascii(CSS::to_string(m_value.get<ReferrerPolicyModifierValue>()));
        break;
    }
    builder.append_ascii(')');
    return builder.to_string();
}

bool RequestURLModifier::operator==(RequestURLModifier const&) const = default;

}
