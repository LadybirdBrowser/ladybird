/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Serializable.h>
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
    : public Bindings::PlatformObject
    , public Bindings::Serializable {
    WEB_PLATFORM_OBJECT(DOMException, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(DOMException);

public:
    static GC::Ref<DOMException> create(JS::Realm& realm, FlyString name, Utf16String const& message);
    static GC::Ref<DOMException> create(JS::Realm& realm);

    // JS constructor has message first, name second
    // FIXME: This is a completely pointless footgun, let's use the same order for both factories.
    static GC::Ref<DOMException> construct_impl(JS::Realm& realm, Utf16String const& message, FlyString name);

    virtual ~DOMException() override;

    FlyString const& name() const { return m_name; }
    Utf16FlyString const& message() const { return m_message; }
    u16 code() const { return get_legacy_code_for_name(m_name); }

    virtual WebIDL::ExceptionOr<void> serialization_steps(HTML::TransferDataEncoder&, bool for_storage, HTML::SerializationMemory&) override;
    virtual WebIDL::ExceptionOr<void> deserialization_steps(HTML::TransferDataDecoder&, HTML::DeserializationMemory&) override;

protected:
    DOMException(JS::Realm&, FlyString name, Utf16String const& message);
    explicit DOMException(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

private:
    FlyString m_name;
    Utf16FlyString m_message;
};

#define __ENUMERATE(ErrorName)                                                            \
    class ErrorName final {                                                               \
    public:                                                                               \
        static GC::Ref<DOMException> create(JS::Realm& realm, Utf16String const& message) \
        {                                                                                 \
            return DOMException::create(realm, #ErrorName##_fly_string, message);         \
        }                                                                                 \
    };
ENUMERATE_DOM_EXCEPTION_ERROR_NAMES
#undef __ENUMERATE

}

namespace Web {

inline JS::Completion throw_completion(GC::Ref<WebIDL::DOMException> exception)
{
    return JS::throw_completion(JS::Value(exception));
}

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
