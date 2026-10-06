/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/EngineCell.h>
#include <LibJS/Position.h>
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

// The [[ErrorData]] of an Error or of an ErrorDataCell: the call stack it was created on. The runtime keeps it inside
// the cell that has it, so a reference to an ErrorData is to the runtime's error data, or for the ErrorData that an
// ErrorDataCell derives from, to the cell itself, which the runtime takes in its place. An Error converts to the
// ErrorData inside it.
class JS_API ErrorData {
    AK_MAKE_NONCOPYABLE(ErrorData);
    AK_MAKE_NONMOVABLE(ErrorData);

public:
    [[nodiscard]] Utf16String stack_string(CompactTraceback compact = CompactTraceback::No) const;

    // The frames are copied out of the runtime, so this returns them by value.
    [[nodiscard]] Vector<TracebackFrame, 32> traceback() const;

protected:
    ErrorData() = delete;
    ~ErrorData() = delete;
};

// The error data of a host object that is not an Error, such as a DOMException, kept in a cell of its own.
class JS_API ErrorDataCell final
    : public EngineCell
    , public ErrorData {
public:
    static GC::Ref<ErrorDataCell> capture(VM&);
};

static_assert(GC::IsForeignCellHandle<ErrorDataCell>);

}
