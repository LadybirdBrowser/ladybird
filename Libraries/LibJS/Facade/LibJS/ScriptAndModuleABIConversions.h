/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibJS/DecodedBytecodeCache.h>
#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Module.h>
#include <LibJS/ModuleLoading.h>
#include <LibJS/ParserError.h>
#include <LibJS/Runtime/ModuleRequest.h>
#include <LibJS/Script.h>
#include <LibJS/SourceCode.h>

// Conversions between the facade's scripts, modules, source code and compiled programs and those of the Rust runtime's
// embedding ABI. Like EmbeddingABIConversions.h, only the facade's own .cpp files may include this header.

namespace JS::EmbeddingABI {

inline JSSourceCode const* source_code_to_abi(SourceCode const& source_code)
{
    return reinterpret_cast<JSSourceCode const*>(&source_code);
}

inline RefPtr<SourceCode const> source_code_from_abi(JSSourceCode const* source_code)
{
    return reinterpret_cast<SourceCode const*>(source_code);
}

// Source code whose one reference the runtime created it with, which the caller takes over.
inline NonnullRefPtr<SourceCode const> adopt_source_code_from_abi(JSSourceCode const* source_code)
{
    VERIFY(source_code);
    return adopt_ref(*reinterpret_cast<SourceCode const*>(source_code));
}

inline JSDecodedBytecodeCache const* decoded_bytecode_cache_to_abi(RustIntegration::DecodedBytecodeCache const& cache)
{
    return reinterpret_cast<JSDecodedBytecodeCache const*>(&cache);
}

// A decoded cache whose one reference the runtime created it with, which the caller takes over.
inline RefPtr<RustIntegration::DecodedBytecodeCache> adopt_decoded_bytecode_cache_from_abi(JSDecodedBytecodeCache const* cache)
{
    if (!cache)
        return {};
    auto& adopted_cache = *reinterpret_cast<RustIntegration::DecodedBytecodeCache*>(const_cast<JSDecodedBytecodeCache*>(cache));
    return adopt_ref(adopted_cache);
}

inline JSProgramType program_type_to_abi(RustIntegration::ProgramType program_type)
{
    return to_underlying(program_type);
}

inline JSRealm* realm_to_abi(Realm const& realm)
{
    return cell_to_abi<JSRealm>(realm);
}

inline JSScript* script_to_abi(Script const& script)
{
    return cell_to_abi<JSScript>(script);
}

inline GC::Ref<Script> script_from_abi(JSScript* script)
{
    VERIFY(script);
    return *cell_from_abi<Script>(script);
}

inline JSModule* module_to_abi(Module const& module)
{
    return cell_to_abi<JSModule>(module);
}

template<typename ModuleType = Module>
GC::Ref<ModuleType> module_from_abi(JSModule* module)
{
    VERIFY(module);
    return *cell_from_abi<ModuleType>(module);
}

// The runtime borrows filenames as UTF-16 and reads them back as UTF-8, which C++ hands over as a StringView.
inline Utf16String filename_to_utf16(StringView filename)
{
    return Utf16String::from_utf8_with_replacement_character(filename, Utf16String::WithBOMHandling::No);
}

// Collects the syntax errors that the runtime reports to a JSParserErrorSink as the C++ runtime's ParserErrors.
class ParserErrorCollector {
    AK_MAKE_NONCOPYABLE(ParserErrorCollector);
    AK_MAKE_NONMOVABLE(ParserErrorCollector);

public:
    ParserErrorCollector() = default;

    JSParserErrorSink const* sink() const { return &m_sink; }
    Vector<ParserError> take_errors() { return move(m_errors); }

private:
    static void append(void* context, JSOwnedUtf16String message, u32 line, u32 column)
    {
        auto& collector = *static_cast<ParserErrorCollector*>(context);
        collector.m_errors.append({ owned_utf16_string_from_abi(message), Position { line, column } });
    }

    Vector<ParserError> m_errors;
    JSParserErrorSink m_sink { .context = this, .append = append };
};

// A script or module record, or the syntax errors that kept the runtime from creating it.
template<typename Record, typename AbiRecord>
Result<GC::Ref<Record>, Vector<ParserError>> record_or_parser_errors_from_abi(AbiRecord* record, ParserErrorCollector& errors)
{
    if (!record) {
        auto parser_errors = errors.take_errors();
        VERIFY(!parser_errors.is_empty());
        return parser_errors;
    }
    return GC::Ref { *cell_from_abi<Record>(record) };
}

// A module request that the facade owns for the duration of a call into the runtime.
class ModuleRequestForABI {
    AK_MAKE_NONCOPYABLE(ModuleRequestForABI);
    AK_MAKE_NONMOVABLE(ModuleRequestForABI);

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit ModuleRequestForABI(ModuleRequest const& module_request)
    {
        Vector<JSImportAttribute> attributes;
        attributes.ensure_capacity(module_request.attributes.size());
        for (auto const& attribute : module_request.attributes)
            attributes.unchecked_append({ utf16_view_to_abi(attribute.key.utf16_view()), utf16_view_to_abi(attribute.value.utf16_view()) });
        m_module_request = js_module_request_create(utf16_view_to_abi(module_request.module_specifier.view()), attributes.data(), attributes.size());
    }

    ~ModuleRequestForABI()
    {
        js_module_request_destroy(m_module_request);
    }

    JSModuleRequest const* ptr() const { return m_module_request; }

private:
    JSModuleRequest* m_module_request { nullptr };
};

// A copy of a module request of the runtime, with its attributes sorted by key, as the C++ runtime keeps them.
inline ModuleRequest module_request_from_abi(JSModuleRequest const& module_request)
{
    auto attribute_count = js_module_request_attribute_count(&module_request);
    Vector<ImportAttribute> attributes;
    attributes.ensure_capacity(attribute_count);
    for (size_t index = 0; index < attribute_count; ++index) {
        auto attribute = js_module_request_attribute(&module_request, index);
        attributes.unchecked_append({ Utf16String::from_utf16(utf16_view_from_abi(attribute.key)), Utf16String::from_utf16(utf16_view_from_abi(attribute.value)) });
    }
    return ModuleRequest { Utf16FlyString::from_utf16(utf16_view_from_abi(js_module_request_specifier(&module_request))), move(attributes) };
}

inline JSImportedModuleReferrer imported_module_referrer_to_abi(ImportedModuleReferrer const& referrer)
{
    return referrer.visit(
        [](GC::Ref<Script> script) { return JSImportedModuleReferrer { JS_IMPORTED_MODULE_REFERRER_SCRIPT, script.ptr() }; },
        [](GC::Ref<CyclicModule> module) { return JSImportedModuleReferrer { JS_IMPORTED_MODULE_REFERRER_CYCLIC_MODULE, module.ptr() }; },
        [](GC::Ref<Realm> realm) { return JSImportedModuleReferrer { JS_IMPORTED_MODULE_REFERRER_REALM, realm.ptr() }; });
}

inline JSImportedModulePayload imported_module_payload_to_abi(ImportedModulePayload const& payload)
{
    return payload.visit(
        [](GC::Ref<GraphLoadingState> state) { return JSImportedModulePayload { JS_IMPORTED_MODULE_PAYLOAD_GRAPH_LOADING_STATE, state.ptr() }; },
        [](GC::Ref<PromiseCapability> capability) { return JSImportedModulePayload { JS_IMPORTED_MODULE_PAYLOAD_PROMISE_CAPABILITY, capability.ptr() }; });
}

// A completion whose normal payload is the module's pointer.
inline JSCompletion module_completion_to_abi(ThrowCompletionOr<GC::Ref<Module>> const& completion)
{
    if (completion.is_throw_completion())
        return { value_to_abi(completion.throw_completion().value()), JS_COMPLETION_THROW };
    return { static_cast<u64>(reinterpret_cast<FlatPtr>(completion.value().ptr())), JS_COMPLETION_NORMAL };
}

}
