/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Utf16String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Module.h>
#include <LibWeb/Bindings/Serializable.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebAssembly/WebAssembly.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebAssembly {

class Module
    : public Bindings::GCAllocatedWrappable
    , public Bindings::Serializable {
    WEB_WRAPPABLE(Module, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(Module);

public:
    static GC::Ref<Module> create(); // Should only be used for structured serialization.
    static WebIDL::ExceptionOr<GC::Ref<Module>> create(JS::Realm&, WebIDL::BufferSource bytes, Bindings::WebAssemblyCompileOptions const&);
    static WebIDL::ExceptionOr<Vector<Bindings::ModuleImportDescriptor>> imports(GC::Ref<Module>);
    static WebIDL::ExceptionOr<Vector<Bindings::ModuleExportDescriptor>> exports(GC::Ref<Module>);
    static WebIDL::ExceptionOr<GC::RootVector<GC::Ref<JS::ArrayBuffer>>> custom_sections(JS::Realm&, GC::Ref<Module>, Utf16String section_name);
    static WebIDL::ExceptionOr<Vector<ByteBuffer>> custom_sections(GC::Ref<Module>, Utf16String section_name);

    NonnullRefPtr<Detail::CompiledWebAssemblyModule> compiled_module() const { return *m_compiled_module; }
    Vector<Utf16String> const& builtin_sets() const { return m_builtin_sets; }
    Optional<Utf16String> const& imported_string_module() const { return m_imported_string_module; }

private:
    Module() = default;
    Module(NonnullRefPtr<Detail::CompiledWebAssemblyModule>, ByteBuffer, Vector<Utf16String> builtin_sets, Optional<Utf16String> imported_string_module);

    virtual WebIDL::ExceptionOr<void> serialization_steps(HTML::StructuredSerializeWriter&, bool for_storage, HTML::SerializationMemory&) override;
    virtual WebIDL::ExceptionOr<void> deserialization_steps(JS::Realm&, HTML::StructuredSerializeReader&, HTML::DeserializationMemory&) override;

    RefPtr<Detail::CompiledWebAssemblyModule> m_compiled_module;
    ByteBuffer m_bytes;
    Vector<Utf16String> m_builtin_sets;
    Optional<Utf16String> m_imported_string_module;
};

}
