/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Utf16View.h>
#include <LibGC/Ptr.h>
#include <LibJS/Embedding/Layout.h>
#include <LibJS/Export.h>
#include <LibJS/Module.h>

namespace JS {

// 16.2.1.8 Synthetic Module Records, https://tc39.es/ecma262/#sec-synthetic-module-records
class JS_API SyntheticModule final : public Module {
public:
    static GC::Ref<SyntheticModule> create_default_export_synthetic_module(Realm& realm, Value default_export, ByteString filename);
};

ThrowCompletionOr<GC::Ref<SyntheticModule>> JS_API parse_json_module(Realm& realm, Utf16View source_text, ByteString filename);
GC::Ref<SyntheticModule> JS_API create_text_module(Realm& realm, Utf16View source, ByteString filename);

}
