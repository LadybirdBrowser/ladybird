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
// the cell that has it, so a reference to an ErrorData is to the runtime's error data, or for the ErrorData of an
// ErrorDataCell, to the cell itself, which the runtime takes in its place. An Error converts to the ErrorData inside
// it.
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
// NB: ErrorDataCell can't derive from ErrorData, as the MSVC ABI places an empty second base after the first one, so
//     the ErrorData would not be at the address of the cell.
class JS_API ErrorDataCell final : public EngineCell {
public:
    static GC::Ref<ErrorDataCell> capture(VM&);

    ErrorData& error_data() { return *reinterpret_cast<ErrorData*>(this); }
    ErrorData const& error_data() const { return *reinterpret_cast<ErrorData const*>(this); }
};

static_assert(GC::IsForeignCellHandle<ErrorDataCell>);

}
