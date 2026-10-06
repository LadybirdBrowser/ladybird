/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibJS/ScriptAndModuleABIConversions.h>
#include <LibJS/SyntheticModule.h>

namespace JS {

using namespace EmbeddingABI;

// 16.2.1.8.1 CreateDefaultExportSyntheticModule ( defaultExport ), https://tc39.es/ecma262/#sec-create-default-export-synthetic-module
GC::Ref<SyntheticModule> SyntheticModule::create_default_export_synthetic_module(Realm& realm, Value default_export, ByteString filename)
{
    auto utf16_filename = filename_to_utf16(filename);
    auto* module = js_module_create_default_export_synthetic_module(vm_to_abi(realm.vm()), realm_to_abi(realm), value_to_abi(default_export), utf16_view_to_abi(utf16_filename.utf16_view()));
    return module_from_abi<SyntheticModule>(module);
}

// 16.2.1.8.2 ParseJSONModule ( source ), https://tc39.es/ecma262/#sec-parse-json-module
ThrowCompletionOr<GC::Ref<SyntheticModule>> parse_json_module(Realm& realm, Utf16View source_text, ByteString filename)
{
    auto utf16_filename = filename_to_utf16(filename);
    return completion_from_abi<GC::Ref<SyntheticModule>>(js_module_parse_json_module(vm_to_abi(realm.vm()), realm_to_abi(realm), utf16_view_to_abi(source_text), utf16_view_to_abi(utf16_filename.utf16_view())));
}

// 16.2.1.8.5 CreateTextModule ( source ), https://tc39.es/proposal-import-text/#sec-create-text-module
GC::Ref<SyntheticModule> create_text_module(Realm& realm, Utf16View source, ByteString filename)
{
    auto utf16_filename = filename_to_utf16(filename);
    auto* module = js_module_create_text_module(vm_to_abi(realm.vm()), realm_to_abi(realm), utf16_view_to_abi(source), utf16_view_to_abi(utf16_filename.utf16_view()));
    return module_from_abi<SyntheticModule>(module);
}

}
