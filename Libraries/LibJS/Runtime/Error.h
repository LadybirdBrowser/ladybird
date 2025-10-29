/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/SourceRange.h>

namespace JS {

struct JS_API TracebackFrame {
    Utf16String function_name;
    [[nodiscard]] SourceRange const& source_range() const;

    RefPtr<CachedSourceRange> cached_source_range;
};

enum CompactTraceback {
    No,
    Yes,
};

class JS_API Error : public Object {
    JS_OBJECT(Error, Object);
    GC_DECLARE_ALLOCATOR(Error);

public:
    static GC::Ref<Error> create(Realm&);
    static GC::Ref<Error> create(Realm&, Utf16String message);
    static GC::Ref<Error> create(Realm&, StringView message);

    virtual ~Error() override = default;

    [[nodiscard]] String stack_string(CompactTraceback compact = CompactTraceback::No) const;

    ThrowCompletionOr<void> install_error_cause(Value options);

    void set_message(Utf16String);

    Vector<TracebackFrame, 32> const& traceback() const { return m_traceback; }

protected:
    explicit Error(Object& prototype);

private:
    virtual bool is_error_object() const final { return true; }

    void populate_stack();
    Vector<TracebackFrame, 32> m_traceback;
};

template<>
inline bool Object::fast_is<Error>() const { return is_error_object(); }

// NOTE: Making these inherit from Error is not required by the spec but
//       our way of implementing the [[ErrorData]] internal slot, which is
//       used in Object.prototype.toString().
#define DECLARE_NATIVE_ERROR(ClassName, snake_name, PrototypeName, ConstructorName) \
    class JS_API ClassName final : public Error {                                   \
        JS_OBJECT(ClassName, Error);                                                \
        GC_DECLARE_ALLOCATOR(ClassName);                                            \
                                                                                    \
    public:                                                                         \
        static GC::Ref<ClassName> create(Realm&);                                   \
        static GC::Ref<ClassName> create(Realm&, Utf16String message);              \
        static GC::Ref<ClassName> create(Realm&, StringView message);               \
                                                                                    \
        explicit ClassName(Object& prototype);                                      \
        virtual ~ClassName() override = default;                                    \
    };

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    DECLARE_NATIVE_ERROR(ClassName, snake_name, PrototypeName, ConstructorName)
JS_ENUMERATE_NATIVE_ERRORS
#undef __JS_ENUMERATE

}
