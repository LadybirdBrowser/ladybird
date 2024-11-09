/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Diagnostics.h>
#include <AK/String.h>
#include <LibWeb/Bindings/PlatformObject.h>
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
    __ENUMERATE(QuotaExceededError)                  \
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
class DOMException final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(DOMException, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(DOMException);

public:
    static JS::NonnullGCPtr<DOMException> create(JS::Realm& realm, FlyString name, String message);

    // JS constructor has message first, name second
    // FIXME: This is a completely pointless footgun, let's use the same order for both factories.
    static JS::NonnullGCPtr<DOMException> construct_impl(JS::Realm& realm, String message, FlyString name);

    virtual ~DOMException() override;

    FlyString const& name() const { return m_name; }
    FlyString const& message() const { return m_message; }
    u16 code() const { return get_legacy_code_for_name(m_name); }

protected:
    DOMException(JS::Realm&, FlyString name, String message);

    virtual void initialize(JS::Realm&) override;

private:
    FlyString m_name;
    FlyString m_message;
};

#define __ENUMERATE(ErrorName)                                                                \
    class ErrorName final {                                                                   \
    public:                                                                                   \
        static JS::NonnullGCPtr<DOMException> create(JS::Realm& realm, String const& message) \
        {                                                                                     \
            return DOMException::create(realm, #ErrorName##_fly_string, message);             \
        }                                                                                     \
    };
ENUMERATE_DOM_EXCEPTION_ERROR_NAMES
#undef __ENUMERATE

}

namespace Web {

inline JS::Completion throw_completion(JS::NonnullGCPtr<WebIDL::DOMException> exception)
{
    return JS::throw_completion(JS::Value(exception));
}

}
