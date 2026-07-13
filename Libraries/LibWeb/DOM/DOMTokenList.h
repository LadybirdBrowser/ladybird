/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#domtokenlist
class DOMTokenList final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(DOMTokenList, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(DOMTokenList);

public:
    [[nodiscard]] static GC::Ref<DOMTokenList> create(Element& associated_element, Utf16FlyString associated_attribute);
    ~DOMTokenList() = default;

    void associated_attribute_changed(Utf16View value);

    virtual Optional<JS::Value> item_value(size_t index) const override;

    size_t length() const { return m_token_set.size(); }
    Optional<Utf16String> item(size_t index) const;
    bool contains(Utf16View token);
    WebIDL::ExceptionOr<void> add(Utf16View token);
    WebIDL::ExceptionOr<void> add(Vector<Utf16String> const& tokens);
    WebIDL::ExceptionOr<void> remove(Utf16View token);
    WebIDL::ExceptionOr<void> remove(Vector<Utf16String> const& tokens);
    WebIDL::ExceptionOr<bool> toggle(Utf16View token, Optional<bool> force);
    WebIDL::ExceptionOr<bool> replace(Utf16View token, Utf16View new_token);
    WebIDL::ExceptionOr<bool> supports(Utf16View token);
    Utf16String value() const;
    void set_value(Utf16View value);

private:
    DOMTokenList(Element& associated_element, Utf16FlyString associated_attribute);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual size_t external_memory_size() const override;

    WebIDL::ExceptionOr<void> validate_token(Utf16View token) const;
    WebIDL::ExceptionOr<void> validate_token_not_empty(Utf16View token) const;
    WebIDL::ExceptionOr<void> validate_token_not_whitespace(Utf16View token) const;
    WebIDL::ExceptionOr<bool> run_validation_steps(Utf16View token);
    void run_update_steps();

    Vector<Utf16String> parse_ordered_set(Utf16View) const;
    Utf16String serialize_ordered_set() const;

    GC::Ref<Element> m_associated_element;
    Utf16FlyString m_associated_attribute;
    Vector<Utf16String> m_token_set;
};

}

struct SupportedTokenKey {
    Utf16FlyString element_name;
    Utf16FlyString attribute_name;

    constexpr bool operator==(SupportedTokenKey const& other) const = default;
};

namespace AK {

template<>
struct Traits<SupportedTokenKey> : public DefaultTraits<SupportedTokenKey> {
    static unsigned hash(SupportedTokenKey const& key)
    {
        return pair_int_hash(key.element_name.hash(), key.attribute_name.hash());
    }
};

}
