/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Result.h>
#include <AK/StringView.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibJS/Export.h>
#include <LibJS/Heap/EngineCell.h>
#include <LibJS/ParserError.h>
#include <LibJS/Runtime/Realm.h>

namespace JS {

// 16.1.4 Script Records, https://tc39.es/ecma262/#sec-script-records
class JS_API Script final : public EngineCell {
public:
    static Result<GC::Ref<Script>, Vector<ParserError>> parse(Utf16View source_text, Realm&, StringView filename = {}, Utf16View display_filename = {}, GC::Ptr<GC::Cell> host_defined = nullptr, size_t line_number_offset = 1);

    GC::Ptr<GC::Cell> host_defined() const;
};

}
