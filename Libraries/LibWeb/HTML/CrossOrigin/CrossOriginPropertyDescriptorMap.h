/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Traits.h>
#include <AK/Utf16FlyString.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Ptr.h>
#include <LibGC/WeakContainer.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/PropertyDescriptor.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

struct CrossOriginProperty {
    Utf16FlyString property;
    Optional<bool> needs_get {};
    Optional<bool> needs_set {};
};

struct CrossOriginKey {
    GC::RawRef<EnvironmentSettingsObject> current_settings_object;
    FlatPtr relevant_settings_object;
    JS::PropertyKey property_key;
};

// The [[CrossOriginPropertyDescriptorMap]] internal slot of a Location or Window object.
// AD-HOC: The functions of a descriptor are of the realm of its key's current settings object, which they keep alive.
//         An entry lives only while something else keeps that settings object alive, so that the map does not keep
//         alive the document of every script that ever read a cross-origin property of the object.
class WEB_API CrossOriginPropertyDescriptorMap final
    : public GC::Cell
    , public GC::WeakContainer {
    GC_CELL(CrossOriginPropertyDescriptorMap, GC::Cell);
    GC_DECLARE_ALLOCATOR(CrossOriginPropertyDescriptorMap);

public:
    using JSValueConversionIsForbidden = void;

    Optional<JS::PropertyDescriptor> get(CrossOriginKey const&) const;
    void set(CrossOriginKey const&, JS::PropertyDescriptor);

private:
    CrossOriginPropertyDescriptorMap();

    virtual void visit_edges(Visitor&) override;
    virtual GC::Cell const& owner_cell(Badge<GC::Heap>) const override { return *this; }
    virtual void remove_dead_cells(Badge<GC::Heap>) override;

    HashMap<CrossOriginKey, JS::PropertyDescriptor> m_descriptors;
};

}

namespace AK {

template<>
struct Traits<Web::HTML::CrossOriginKey> : public DefaultTraits<Web::HTML::CrossOriginKey> {
    static unsigned hash(Web::HTML::CrossOriginKey const& key)
    {
        return pair_int_hash(
            Traits<JS::PropertyKey>::hash(key.property_key),
            pair_int_hash(ptr_hash(key.current_settings_object.ptr()), ptr_hash(key.relevant_settings_object)));
    }

    static bool equals(Web::HTML::CrossOriginKey const& a, Web::HTML::CrossOriginKey const& b)
    {
        return a.current_settings_object == b.current_settings_object
            && a.relevant_settings_object == b.relevant_settings_object
            && Traits<JS::PropertyKey>::equals(a.property_key, b.property_key);
    }
};

}
