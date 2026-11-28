/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Alexander Narsudinov <a.narsudinov@gmail.com>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/RustFFI.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#interface-namednodemap
class WEB_API NamedNodeMap : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(NamedNodeMap, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(NamedNodeMap);

public:
    [[nodiscard]] static GC::Ref<NamedNodeMap> create(Element&);
    // NB: The buffers of the core are freed when the cell is swept, not when it is finalized, as the C++ members that
    //     held them were, so that the finalizer of another dead cell can still read them.
    GC_ALLOW_CELL_DESTRUCTOR virtual ~NamedNodeMap() override;

    virtual Vector<Utf16FlyString> supported_property_names() const override;
    virtual bool is_supported_property_name(Utf16FlyString const&) const override;

    size_t length() const;
    bool is_empty() const { return length() == 0; }

    // Methods defined by the spec for JavaScript:
    GC::Ptr<Attr> item(u32 index) const;
    GC::Ptr<Attr> get_named_item(Utf16FlyString const& qualified_name) const;
    GC::Ptr<Attr> get_named_item_ns(Optional<Utf16FlyString> const& namespace_, Utf16FlyString const& local_name) const;
    WebIDL::ExceptionOr<GC::Ptr<Attr>> set_named_item(Attr& attribute);
    WebIDL::ExceptionOr<GC::Ptr<Attr>> set_named_item_ns(Attr& attribute);
    WebIDL::ExceptionOr<GC::Ref<Attr>> remove_named_item(Utf16FlyString const& qualified_name);
    WebIDL::ExceptionOr<GC::Ref<Attr>> remove_named_item_ns(Optional<Utf16FlyString> const& namespace_, Utf16FlyString const& local_name);

    // Methods defined by the spec for internal use:
    GC::Ptr<Attr> get_attribute(Utf16FlyString const& qualified_name, size_t* item_index = nullptr) const;
    WebIDL::ExceptionOr<GC::Ptr<Attr>> set_attribute(GC::Ref<Attr> attribute);
    void replace_attribute(GC::Ref<Attr> old_attribute, GC::Ref<Attr> new_attribute, size_t old_attribute_index);
    void append_attribute(GC::Ref<Attr> attribute);
    void move_attribute_nodes_to_document(Badge<Element>, Document&);

    GC::Ptr<Attr> get_attribute_ns(Optional<Utf16FlyString> const& namespace_, Utf16FlyString const& local_name, size_t* item_index = nullptr) const;

    GC::Ptr<Attr> remove_attribute(Utf16FlyString const& qualified_name);
    GC::Ptr<Attr> remove_attribute_ns(Optional<Utf16FlyString> const& namespace_, Utf16FlyString const& local_name);

    WebIDL::ExceptionOr<GC::Ref<Attr>> remove_attribute_node(GC::Ref<Attr>);

private:
    explicit NamedNodeMap(Element&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    Element& associated_element() { return *m_element; }
    Element const& associated_element() const { return *m_element; }

    void remove_attribute_at_index(size_t attribute_index);
    GC::Ptr<Attr> find_attribute_node(QualifiedName const&) const;
    GC::Ref<Attr> ensure_attribute_node(QualifiedName const&) const;

    ReadonlySpan<GC::Member<Node>> attribute_nodes() const { return { m_core.attribute_nodes.items, m_core.attribute_nodes.size }; }

    GC::NonnullMember<DOM::Element> m_element;

    // The Attr nodes the map made, whose layout libweb_rust defines (see Rust/src/dom/attr.rs), which detaches them
    // from the element as their attributes go.
    RustFFI::NamedNodeMapCore m_core {};
};

}
