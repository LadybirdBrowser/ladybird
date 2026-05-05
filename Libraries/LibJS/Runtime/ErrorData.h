/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/SourceRange.h>

namespace JS {

struct JS_API TracebackFrame {
    Utf16String function_name;
    [[nodiscard]] SourceRange const& source_range() const;

    Optional<SourceRange> cached_source_range;
};

enum CompactTraceback {
    No,
    Yes,
};

class JS_API ErrorData {
public:
    explicit ErrorData(VM&);

    [[nodiscard]] Utf16String stack_string(CompactTraceback compact = CompactTraceback::No) const;
    [[nodiscard]] Vector<TracebackFrame, 32> const& traceback() const { return m_traceback; }

    void set_cached_string(GC::Ref<PrimitiveString> string) { m_cached_string = string; }
    [[nodiscard]] GC::Ptr<PrimitiveString> cached_string() const { return m_cached_string; }

protected:
    void visit_edges(Cell::Visitor&);
    size_t external_memory_size() const;

private:
    void populate_stack(VM&);

    Vector<TracebackFrame, 32> m_traceback;
    GC::Ptr<PrimitiveString> m_cached_string;
};

}
