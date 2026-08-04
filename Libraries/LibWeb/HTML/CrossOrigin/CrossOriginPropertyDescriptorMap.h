/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Optional.h>
#include <AK/Traits.h>
#include <AK/Utf16FlyString.h>
#include <LibGC/Cell.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/PropertyDescriptor.h>
#include <LibJS/Runtime/PropertyKey.h>

namespace Web::HTML {

struct CrossOriginProperty {
    Utf16FlyString property;
    Optional<bool> needs_get {};
    Optional<bool> needs_set {};
};

struct CrossOriginKey {
    FlatPtr current_settings_object;
    FlatPtr relevant_settings_object;
    JS::PropertyKey property_key;
};

struct CrossOriginCachedPropertyDescriptor {
    explicit CrossOriginCachedPropertyDescriptor(JS::PropertyDescriptor descriptor)
        : descriptor(move(descriptor))
    {
    }

    void visit_edges(GC::Cell::Visitor& visitor) { descriptor.visit_edges(visitor); }

    JS::PropertyDescriptor descriptor;
};

using CrossOriginPropertyDescriptorMap = HashMap<CrossOriginKey, CrossOriginCachedPropertyDescriptor>;

}

namespace AK {

template<>
struct Traits<Web::HTML::CrossOriginKey> : public DefaultTraits<Web::HTML::CrossOriginKey> {
    static unsigned hash(Web::HTML::CrossOriginKey const& key)
    {
        return pair_int_hash(
            Traits<JS::PropertyKey>::hash(key.property_key),
            pair_int_hash(ptr_hash(key.current_settings_object), ptr_hash(key.relevant_settings_object)));
    }

    static bool equals(Web::HTML::CrossOriginKey const& a, Web::HTML::CrossOriginKey const& b)
    {
        return a.current_settings_object == b.current_settings_object
            && a.relevant_settings_object == b.relevant_settings_object
            && Traits<JS::PropertyKey>::equals(a.property_key, b.property_key);
    }
};

}
