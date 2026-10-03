/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/CellAllocator.h>
#include <LibJS/CyclicModule.h>
#include <LibJS/Export.h>
#include <LibJS/HostObjectABI.h>
#include <LibJS/Runtime/ModuleRequest.h>

namespace JS {

// A Cyclic Module Record whose abstract methods come from a JSHostClass of kind JS_HOST_CLASS_MODULE, with a companion
// cell for any state of its own. It never has top-level await.
class JS_API HostModule : public CyclicModule {
    GC_CELL_WITH_CUSTOM_CLASS_NAME(HostModule, CyclicModule);

public:
    static GC::Ref<HostModule> create(Realm&, JSHostClass const&, StringView filename, Vector<ModuleRequest> requested_modules, GC::Ptr<GC::Cell> host_defined = {}, GC::Ptr<GC::Cell> host_data = {});

    virtual ~HostModule() override = default;

    JSHostClass const& host_class() const { return *m_host_class; }
    GC::Ptr<GC::Cell> host_data() const { return m_host_data; }
    void set_host_data(GC::Ptr<GC::Cell> host_data) { m_host_data = host_data; }

    virtual StringView class_name() const override;

    using Module::get_exported_names;
    virtual Vector<Utf16FlyString> get_exported_names(VM&, GC::RootHashTable<GC::Ref<Module const>>& export_star_set) override;
    virtual ResolvedBinding resolve_export(VM&, Utf16FlyString const& export_name, Vector<ResolvedBinding> resolve_set = {}) override;

    // What the hooks need from the module record to set up and run the module.
    using CyclicModule::get_imported_module;
    using Module::set_environment;

protected:
    HostModule(Realm&, JSHostClass const&, StringView filename, Vector<ModuleRequest> requested_modules, GC::Ptr<GC::Cell> host_defined, GC::Ptr<GC::Cell> host_data);

    virtual void visit_edges(Cell::Visitor&) override;

    virtual ThrowCompletionOr<void> initialize_environment(VM&) override;
    virtual ThrowCompletionOr<void> execute_module(VM&, GC::Ptr<PromiseCapability>) override;

private:
    JSHostModuleHooks const& hooks() const;

    JSHostClass const* m_host_class { nullptr };
    GC::Ptr<GC::Cell> m_host_data;
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
