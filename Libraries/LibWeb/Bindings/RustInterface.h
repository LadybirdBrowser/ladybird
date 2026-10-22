/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/StringView.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibJS/HostObjectABI.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibWeb/Bindings/IntrinsicDefinitions.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Bindings {

// The cell of an object of an interface that is implemented in Rust, which the wrappers of the object hold as their
// host data. The Rust bindings in Libraries/LibWeb/Bindings/Rust create these cells and know what they hold.
//
// C++ code that holds or receives objects of such an interface names them through a class that derives from this
// one and uses WEB_RUST_INTERFACE. The class has the name and the header that the implementation class of an
// interface implemented in C++ would have, so the C++ bindings of the interfaces that name the interface convert
// its objects as they convert any other: with impl_from() and wrap(), which are overloaded for these classes below.
class RustInterfaceCell : public GC::ForeignCell {
};

// The class that the wrapper class of every interface implemented in Rust derives from.
extern "C" JSHostClass const web_bindings_rust_interface_wrapper_host_class;

// Says which interface a class that derives from RustInterfaceCell names the objects of. The generated header
// LibWeb/Bindings/<class_>.h, which the header of the class includes, declares the class of the wrappers.
#define WEB_RUST_INTERFACE(class_, snake_name)                                       \
public:                                                                              \
    static constexpr auto s_interface_name = ::Web::Bindings::InterfaceName::class_; \
    static JSHostClass const& wrapper_host_class()                                   \
    {                                                                                \
        return ::Web::Bindings::web_bindings_##snake_name##_wrapper_host_class;      \
    }                                                                                \
                                                                                     \
private:

template<typename T>
concept RustInterfaceHandle = IsBaseOf<RustInterfaceCell, T> && GC::IsForeignCellHandle<T> && requires {
    { T::s_interface_name } -> ConvertibleTo<InterfaceName>;
    { T::wrapper_host_class() } -> SameAs<JSHostClass const&>;
};

WEB_API GC::Ref<JS::HostObject> wrap_rust_interface_object(WrapperWorld&, JS::Realm& preferred_realm, InterfaceName, RustInterfaceCell&);

// The cell of the object, if it is a wrapper of exactly the interface whose wrappers are of the class, and not of an
// interface that inherits from it.
WEB_API RustInterfaceCell* rust_interface_cell_of_wrapper(JS::Object&, JSHostClass const& wrapper_host_class);

// Whether the object is a wrapper of an object of an interface implemented in Rust that is or inherits from the
// interface with the name, as Wrappable::implements_interface() says for the objects of the interfaces implemented in
// C++.
WEB_API bool rust_interface_object_implements_interface(JS::Object const&, StringView interface_name);

template<RustInterfaceHandle T>
[[nodiscard]] GC::Ref<JS::HostObject> wrap(WrapperWorld& wrapper_world, JS::Realm& preferred_realm, GC::Ref<T> object)
{
    return wrap_rust_interface_object(wrapper_world, preferred_realm, T::s_interface_name, *object);
}

template<RustInterfaceHandle T>
[[nodiscard]] GC::Ptr<JS::HostObject> wrap(WrapperWorld& wrapper_world, JS::Realm& preferred_realm, GC::Ptr<T> object)
{
    if (!object)
        return nullptr;
    return wrap_rust_interface_object(wrapper_world, preferred_realm, T::s_interface_name, *object);
}

// The object that a wrapper of the interface wraps. The cell of an object of an interface implemented in Rust that
// others inherit from can be that of an object of any of them, which the C ABI of the interface accepts.
template<RustInterfaceHandle Impl, typename Object>
[[nodiscard]] CopyConst<Object, Impl>* impl_from(Object* object)
{
    if (!object || !JS::is_host_instance_of(*object, Impl::wrapper_host_class()))
        return nullptr;
    return GC::static_cell_cast<Impl>(JS::host_data_of(*object).ptr());
}

}
