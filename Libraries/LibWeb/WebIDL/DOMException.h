/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/ErrorData.h>
#include <LibWeb/Bindings/Serializable.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Scripting/Environments.h>

namespace Web::WebIDL {

// The following have a legacy code value but *don't* produce it as
// DOMException.code value when used as name (and are therefore omitted here):
// - DOMStringSizeError (DOMSTRING_SIZE_ERR = 2)
// - NoDataAllowedError (NO_DATA_ALLOWED_ERR = 6)
// - ValidationError (VALIDATION_ERR = 16)
#define ENUMERATE_DOM_EXCEPTION_LEGACY_CODES   \
    __ENUMERATE(IndexSizeError, 1)             \
    __ENUMERATE(HierarchyRequestError, 3)      \
    __ENUMERATE(WrongDocumentError, 4)         \
    __ENUMERATE(InvalidCharacterError, 5)      \
    __ENUMERATE(NoModificationAllowedError, 7) \
    __ENUMERATE(NotFoundError, 8)              \
    __ENUMERATE(NotSupportedError, 9)          \
    __ENUMERATE(InUseAttributeError, 10)       \
    __ENUMERATE(InvalidStateError, 11)         \
    __ENUMERATE(SyntaxError, 12)               \
    __ENUMERATE(InvalidModificationError, 13)  \
    __ENUMERATE(NamespaceError, 14)            \
    __ENUMERATE(InvalidAccessError, 15)        \
    __ENUMERATE(TypeMismatchError, 17)         \
    __ENUMERATE(SecurityError, 18)             \
    __ENUMERATE(NetworkError, 19)              \
    __ENUMERATE(AbortError, 20)                \
    __ENUMERATE(URLMismatchError, 21)          \
    __ENUMERATE(QuotaExceededError, 22)        \
    __ENUMERATE(TimeoutError, 23)              \
    __ENUMERATE(InvalidNodeTypeError, 24)      \
    __ENUMERATE(DataCloneError, 25)

// https://webidl.spec.whatwg.org/#idl-DOMException-error-names
// Same order as in the spec document, also matches the legacy codes order above.
// Omits QuotaExceededError as that has it's own DOMException derived interface.
#define ENUMERATE_DOM_EXCEPTION_ERROR_NAMES          \
    __ENUMERATE(IndexSizeError) /* Deprecated */     \
    __ENUMERATE(HierarchyRequestError)               \
    __ENUMERATE(WrongDocumentError)                  \
    __ENUMERATE(InvalidCharacterError)               \
    __ENUMERATE(NoModificationAllowedError)          \
    __ENUMERATE(NotFoundError)                       \
    __ENUMERATE(NotSupportedError)                   \
    __ENUMERATE(InUseAttributeError)                 \
    __ENUMERATE(InvalidStateError)                   \
    __ENUMERATE(SyntaxError)                         \
    __ENUMERATE(InvalidModificationError)            \
    __ENUMERATE(NamespaceError)                      \
    __ENUMERATE(InvalidAccessError) /* Deprecated */ \
    __ENUMERATE(TypeMismatchError)  /* Deprecated */ \
    __ENUMERATE(SecurityError)                       \
    __ENUMERATE(NetworkError)                        \
    __ENUMERATE(AbortError)                          \
    __ENUMERATE(URLMismatchError)                    \
    __ENUMERATE(TimeoutError)                        \
    __ENUMERATE(InvalidNodeTypeError)                \
    __ENUMERATE(DataCloneError)                      \
    __ENUMERATE(EncodingError)                       \
    __ENUMERATE(NotReadableError)                    \
    __ENUMERATE(UnknownError)                        \
    __ENUMERATE(ConstraintError)                     \
    __ENUMERATE(DataError)                           \
    __ENUMERATE(TransactionInactiveError)            \
    __ENUMERATE(ReadOnlyError)                       \
    __ENUMERATE(VersionError)                        \
    __ENUMERATE(OperationError)                      \
    __ENUMERATE(NotAllowedError)

static u16 get_legacy_code_for_name(FlyString const& name)
{
#define __ENUMERATE(ErrorName, code) \
    if (name == #ErrorName)          \
        return code;
    ENUMERATE_DOM_EXCEPTION_LEGACY_CODES
#undef __ENUMERATE
    return 0;
}

// https://webidl.spec.whatwg.org/#idl-DOMException
class WEB_API DOMException
    : public Bindings::Wrappable
    , public JS::ErrorData
    , public Bindings::Serializable {
    WEB_WRAPPABLE(DOMException, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(DOMException);

public:
    static GC::Ref<DOMException> create(FlyString name, Utf16String const& message);
    static GC::Ref<DOMException> create();
    static GC::Ref<DOMException> create_for_constructor(Utf16String const& message, FlyString const& name) { return create(name, message); }

    virtual ~DOMException() override;

    FlyString const& name() const { return m_name; }
    Utf16FlyString const& message() const { return m_message; }
    u16 code() const { return get_legacy_code_for_name(m_name); }

    virtual WebIDL::ExceptionOr<void> serialization_steps(JS::Realm&, HTML::TransferDataEncoder&, bool for_storage, HTML::SerializationMemory&) override;
    virtual WebIDL::ExceptionOr<void> deserialization_steps(JS::Realm&, HTML::TransferDataDecoder&, HTML::DeserializationMemory&) override;

protected:
    DOMException(FlyString name, Utf16String const& message);
    DOMException();

    virtual void visit_edges(GC::Cell::Visitor&) override;

    FlyString m_name;
    Utf16FlyString m_message;
};

#define __ENUMERATE(ErrorName)                                                      \
    class ErrorName final {                                                         \
    public:                                                                         \
        static GC::Ref<DOMException> create(Utf16String const& message)             \
        {                                                                           \
            return DOMException::create(#ErrorName##_fly_string, message);          \
        }                                                                           \
        static GC::Ref<DOMException> create(JS::Realm&, Utf16String const& message) \
        {                                                                           \
            return DOMException::create(#ErrorName##_fly_string, message);          \
        }                                                                           \
    };
ENUMERATE_DOM_EXCEPTION_ERROR_NAMES
#undef __ENUMERATE

}

namespace Web {

WEB_API JS::Completion throw_completion(JS::Realm&, GC::Ref<WebIDL::DOMException> exception);

}

namespace Web::Bindings {

struct DOMExceptionReportDetails {
    String name;
    String message;
};

WEB_API WebIDL::DOMException* dom_exception_from_object(JS::Object&);
WEB_API Optional<DOMExceptionReportDetails> dom_exception_report_details(JS::Object&);

}

namespace AK {

template<>
struct Formatter<Web::WebIDL::DOMException> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Web::WebIDL::DOMException const& exception)
    {
        return Formatter<FormatString>::format(builder, "[{}]: {}"sv, exception.name(), exception.message());
    }
};

}
