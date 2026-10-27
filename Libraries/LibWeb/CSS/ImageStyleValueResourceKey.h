/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashFunctions.h>
#include <AK/Optional.h>
#include <AK/Traits.h>
#include <AK/Utf16FlyString.h>
#include <LibURL/URL.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/URL.h>

namespace Web::CSS {

// NB: Identifies the image request of a url() value. Besides the URL, this holds the values of the URL request
//     modifiers, which set the mode, credentials mode, integrity metadata and referrer policy of the request. Only
//     url() values with the same key share an ImageStyleValueResource, so that each of them gets the checks for its
//     own kind of request.
struct ImageStyleValueResourceKey {
    ::URL::URL url;
    Optional<CrossOriginModifierValue> cross_origin;
    Optional<Utf16FlyString> integrity;
    Optional<ReferrerPolicyModifierValue> referrer_policy;

    static ImageStyleValueResourceKey create(::URL::URL url, URL const& url_value)
    {
        ImageStyleValueResourceKey key { .url = move(url), .cross_origin = {}, .integrity = {}, .referrer_policy = {} };

        // NB: The modifier steps run in sequence, so a later modifier of a kind overrides an earlier one.
        for (auto const& modifier : url_value.request_url_modifiers()) {
            switch (modifier.type()) {
            case RequestURLModifier::Type::CrossOrigin:
                key.cross_origin = modifier.value().get<CrossOriginModifierValue>();
                break;
            case RequestURLModifier::Type::Integrity:
                key.integrity = modifier.value().get<Utf16FlyString>();
                break;
            case RequestURLModifier::Type::ReferrerPolicy:
                key.referrer_policy = modifier.value().get<ReferrerPolicyModifierValue>();
                break;
            }
        }
        return key;
    }

    bool has_request_url_modifiers() const { return cross_origin.has_value() || integrity.has_value() || referrer_policy.has_value(); }

    bool operator==(ImageStyleValueResourceKey const&) const = default;
};

}

template<>
struct AK::Traits<Web::CSS::ImageStyleValueResourceKey> : public AK::DefaultTraits<Web::CSS::ImageStyleValueResourceKey> {
    static unsigned hash(Web::CSS::ImageStyleValueResourceKey const& key)
    {
        auto hash = Traits<URL::URL>::hash(key.url);
        hash = pair_int_hash(hash, key.cross_origin.has_value() ? to_underlying(*key.cross_origin) + 1 : 0);
        hash = pair_int_hash(hash, key.integrity.has_value() ? key.integrity->hash() : 0);
        hash = pair_int_hash(hash, key.referrer_policy.has_value() ? to_underlying(*key.referrer_policy) + 1 : 0);
        return hash;
    }
};
