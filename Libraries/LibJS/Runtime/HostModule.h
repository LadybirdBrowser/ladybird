/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StdLibExtras.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/CyclicModule.h>
#include <LibJS/Embedding/Layout.h>
#include <LibJS/Export.h>
#include <LibJS/HostObjectABI.h>
#include <LibJS/Runtime/ModuleRequest.h>

namespace JS {

// A Cyclic Module Record whose abstract methods come from a JSHostClass of kind JS_HOST_CLASS_MODULE, with a companion
// cell for any state of its own. It never has top-level await.
class JS_API HostModule : public CyclicModule {
public:
    static GC::Ref<HostModule> create(Realm&, JSHostClass const&, StringView filename, Vector<ModuleRequest> requested_modules, GC::Ptr<GC::Cell> host_defined = {}, GC::Ptr<GC::Cell> host_data = {});

    JSHostClass const& host_class() const;
    GC::Ptr<GC::Cell> host_data() const;

    StringView class_name() const;

    // What the hooks need from the module record to set up and run the module.
    using CyclicModule::get_imported_module;
    using Module::set_environment;
};

// The module's host class, or null for a module record that the engine implements.
JS_API JSHostClass const* host_class_of(Module const&);

// Whether the module's host class is the given one or derives from it through JSHostClass::parent.
JS_API bool is_host_instance_of(Module const&, JSHostClass const&);

// The module's companion cell if it is exactly a T, like host_data_if() for objects.
template<typename T>
T* host_data_if(Module const& module)
{
    static_assert(IsSame<typename decltype(T::cell_allocator)::CellType, T>, "T must declare its own allocator");
    if (!host_class_of(module))
        return nullptr;
    auto host_data = static_cast<HostModule const&>(module).host_data();
    if (!host_data || !GC::cell_was_allocated_from(*host_data, T::cell_allocator))
        return nullptr;
    return static_cast<T*>(host_data.ptr());
}

}
