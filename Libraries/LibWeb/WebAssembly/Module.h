/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Array.h>
#include <LibWasm/Types.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Module.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebAssembly/WebAssembly.h>

namespace Web::HTML {

class WindowOrWorkerGlobalScopeMixin;

}

namespace Web::WebAssembly {

class Module : public Bindings::Wrappable {
    WEB_WRAPPABLE(Module, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(Module);

public:
    static WebIDL::ExceptionOr<GC::Ref<Module>> construct_impl(HTML::WindowOrWorkerGlobalScopeMixin&, WebIDL::BufferSource bytes);
    static WebIDL::ExceptionOr<Vector<Bindings::ModuleImportDescriptor>> imports(JS::VM&, GC::Ref<Module>);
    static WebIDL::ExceptionOr<Vector<Bindings::ModuleExportDescriptor>> exports(JS::VM&, GC::Ref<Module>);
    static WebIDL::ExceptionOr<GC::RootVector<GC::Ref<JS::ArrayBuffer>>> custom_sections(JS::Realm&, GC::Ref<Module>, String section_name);

    NonnullRefPtr<Detail::CompiledWebAssemblyModule> compiled_module() const { return m_compiled_module; }

private:
    explicit Module(NonnullRefPtr<Detail::CompiledWebAssemblyModule>);

    NonnullRefPtr<Detail::CompiledWebAssemblyModule> m_compiled_module;
};

}
