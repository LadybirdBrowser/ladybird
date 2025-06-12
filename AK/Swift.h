/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if __has_include(<swift/bridging>)
#    include <swift/bridging>
// FIXME: Workaround for Xcode 14/15. When swif becomes required, we should bump the
//        required Xcode verison to one that supports all the features we are using.
#    ifndef SWIFT_UNCHECKED_SENDABLE
#        define SWIFT_UNCHECKED_SENDABLE
#        define SWIFT_NONCOPYABLE
#        define SWIFT_NONESCAPABLE
#        define SWIFT_ESCAPABLE
#        define SWIFT_ESCAPABLE_IF(...)
#        define SWIFT_RETURNS_RETAINED
#        define SWIFT_RETURNS_UNRETAINED
#    endif
#else
#    define SWIFT_SELF_CONTAINED
#    define SWIFT_RETURNS_INDEPENDENT_VALUE
#    define SWIFT_SHARED_REFERENCE(retain, release)
#    define SWIFT_IMMORTAL_REFERENCE
#    define SWIFT_UNSAFE_REFERENCE
#    define SWIFT_NAME(name)
#    define SWIFT_CONFORMS_TO_PROTOCOL(protocol)
#    define SWIFT_COMPUTED_PROPERTY
#    define SWIFT_MUTATING
#    define SWIFT_UNCHECKED_SENDABLE
#    define SWIFT_NONCOPYABLE
#    define SWIFT_NONESCAPABLE
#    define SWIFT_ESCAPABLE
#    define SWIFT_ESCAPABLE_IF(...)
#    define SWIFT_RETURNS_RETAINED
#    define SWIFT_RETURNS_UNRETAINED
#endif

// FIXME: This needs to be in the global namespace for reasons
//  https://github.com/swiftlang/swift/issues/80231
template<class From, class To>
To cxxCast(From i)
{
    return static_cast<To>(i);
}
