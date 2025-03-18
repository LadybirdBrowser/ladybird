/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringView.h>

#define JS_ENUMERATE_ERROR_TYPES(M)                                                                                                 \
    M(AccessorBadField, "Accessor descriptor's '{}' field must be a function or undefined")                                         \
    M(AccessorValueOrWritable, "Accessor property descriptor cannot specify a value or writable key")                               \
    M(AgentCannotSuspend, "Agent is not allowed to suspend")                                                                        \
    M(ArrayMaxSize, "Maximum array size exceeded")                                                                                  \
    M(AsyncDisposableStackAlreadyDisposed, "AsyncDisposableStack is already disposed")                                              \
    M(BadArgCountMany, "{}() needs {} arguments")                                                                                   \
    M(BadArgCountOne, "{}() needs one argument")                                                                                    \
    M(BigIntBadOperator, "Cannot use {} operator with BigInt")                                                                      \
    M(BigIntBadOperatorOtherType, "Cannot use {} operator with BigInt and other type")                                              \
    M(BigIntFromNonIntegral, "Cannot convert non-integral number to BigInt")                                                        \
    M(BigIntInvalidValue, "Invalid value for BigInt: {}")                                                                           \
    M(BindingNotInitialized, "Binding {} is not initialized")                                                                       \
    M(BufferOutOfBounds, "{} contains a property which references a value at an index not contained within its buffer's bounds")    \
    M(ByteLengthExceedsMaxByteLength, "ArrayBuffer byte length of {} exceeds the max byte length of {}")                            \
    M(CallStackSizeExceeded, "Call stack size limit exceeded")                                                                      \
    M(CannotBeHeldWeakly, "{} cannot be held weakly")                                                                               \
    M(CannotDeclareGlobalFunction, "Cannot declare global function of name '{}'")                                                   \
    M(CannotDeclareGlobalVariable, "Cannot declare global variable of name '{}'")                                                   \
    M(ClassConstructorWithoutNew, "Class constructor {} must be called with 'new'")                                                 \
    M(ClassExtendsValueInvalidPrototype, "Class extends value has an invalid prototype {}")                                         \
    M(ClassExtendsValueNotAConstructorOrNull, "Class extends value {} is not a constructor or null")                                \
    M(ClassIsAbstract, "Abstract class {} cannot be constructed directly")                                                          \
    M(ConstructorWithoutNew, "{} constructor must be called with 'new'")                                                            \
    M(Convert, "Cannot convert {} to {}")                                                                                           \
    M(DataViewOutOfRangeByteOffset, "Data view byte offset {} is out of range for buffer with length {}")                           \
    M(DerivedConstructorReturningInvalidValue, "Derived constructor return invalid value")                                          \
    M(DescWriteNonWritable, "Cannot write to non-writable property '{}'")                                                           \
    M(DetachedArrayBuffer, "ArrayBuffer is detached")                                                                               \
    M(DetachKeyMismatch, "Provided detach key {} does not match the ArrayBuffer's detach key {}")                                   \
    M(DisposableStackAlreadyDisposed, "DisposableStack already disposed values")                                                    \
    M(DivisionByZero, "Division by zero")                                                                                           \
    M(DynamicImportNotAllowed, "Dynamic Imports are not allowed")                                                                   \
    M(FinalizationRegistrySameTargetAndValue, "Target and held value must not be the same")                                         \
    M(FixedArrayBuffer, "ArrayBuffer is not resizable")                                                                             \
    M(GeneratorAlreadyExecuting, "Generator is already executing")                                                                  \
    M(GeneratorBrandMismatch, "Generator brand '{}' does not match generator brand '{}')")                                          \
    M(GetCapabilitiesExecutorCalledMultipleTimes, "GetCapabilitiesExecutor was called multiple times")                              \
    M(GetLegacyRegExpStaticPropertyThisValueMismatch,                                                                               \
        "Legacy RegExp static property getter must be called with the RegExp constructor for the this value")                       \
    M(GetLegacyRegExpStaticPropertyValueEmpty, "Legacy RegExp static property getter value is empty")                               \
    M(GlobalEnvironmentAlreadyHasBinding, "Global environment already has binding '{}'")                                            \
    M(IndexOutOfRange, "Index {} is out of range of array length {}")                                                               \
    M(InOperatorWithObject, "'in' operator must be used on an object")                                                              \
    M(InstanceOfOperatorBadPrototype, "'prototype' property of {} is not an object")                                                \
    M(IntlFractionalUnitFollowedByNonFractionalUnit, "Non-fractional unit {} is not allowed after a fractional unit")               \
    M(IntlFractionalUnitsMixedWithAlwaysDisplay, "Fractional unit {} may not be used with {} value of 'always'")                    \
    M(IntlInvalidDateTimeFormatOption, "Option {} cannot be set when also providing {}")                                            \
    M(IntlInvalidKey, "{} is not a valid key")                                                                                      \
    M(IntlInvalidLanguageTag, "{} is not a structurally valid language tag")                                                        \
    M(IntlInvalidRoundingIncrement, "{} is not a valid rounding increment")                                                         \
    M(IntlInvalidRoundingIncrementForFractionDigits, "{} is not a valid rounding increment for inequal min/max fraction digits")    \
    M(IntlInvalidRoundingIncrementForRoundingType, "{} is not a valid rounding increment for rounding type {}")                     \
    M(IntlInvalidTime, "Time value must be between -8.64E15 and 8.64E15")                                                           \
    M(IntlInvalidUnit, "Unit {} is not a valid time unit")                                                                          \
    M(IntlMinimumExceedsMaximum, "Minimum value {} is larger than maximum value {}")                                                \
    M(IntlNonNumericOr2DigitAfterNumericOr2Digit,                                                                                   \
        "Styles other than 'fractional', numeric', or '2-digit' may not be used in smaller units after being used in larger units") \
    M(IntlNumberIsNaNOrOutOfRange, "Value {} is NaN or is not between {} and {}")                                                   \
    M(IntlOptionUndefined, "Option {} must be defined when option {} is {}")                                                        \
    M(IntlTemporalFormatIsNull, "Unable to determine format for {}")                                                                \
    M(IntlTemporalFormatRangeTypeMismatch, "Cannot format a date-time range with different date-time types")                        \
    M(IntlTemporalInvalidCalendar, "Cannot format {} with calendar '{}' in locale with calendar '{}'")                              \
    M(IntlTemporalZonedDateTime, "Cannot format Temporal.ZonedDateTime, use Temporal.ZonedDateTime.prototype.toLocaleString")       \
    M(InvalidAssignToConst, "Invalid assignment to const variable")                                                                 \
    M(InvalidCodePoint, "Invalid code point {}, must be an integer no less than 0 and no greater than 0x10FFFF")                    \
    M(InvalidEnumerationValue, "Invalid value '{}' for enumeration type '{}'")                                                      \
    M(InvalidFractionDigits, "Fraction Digits must be an integer no less than 0, and no greater than 100")                          \
    M(InvalidHint, "Invalid hint: \"{}\"")                                                                                          \
    M(InvalidIndex, "Index must be a positive integer no greater than 2^53-1")                                                      \
    M(InvalidLeftHandAssignment, "Invalid left-hand side in assignment")                                                            \
    M(InvalidLength, "Invalid {} length")                                                                                           \
    M(InvalidNormalizationForm, "The normalization form must be one of NFC, NFD, NFKC, NFKD. Got '{}'")                             \
    M(InvalidOrAmbiguousExportEntry, "Invalid or ambiguous export entry '{}'")                                                      \
    M(InvalidPrecision, "Precision must be an integer no less than 1, and no greater than 100")                                     \
    M(InvalidRadix, "Radix must be an integer no less than 2, and no greater than 36")                                              \
    M(InvalidRestrictedFloatingPointParameter, "Expected {} to be a finite floating-point number")                                  \
    M(InvalidTimeValue, "Invalid time value")                                                                                       \
    M(IsNotA, "{} is not a {}")                                                                                                     \
    M(IsNotAEvaluatedFrom, "{} is not a {} (evaluated from '{}')")                                                                  \
    M(IsNotAn, "{} is not an {}")                                                                                                   \
    M(IsUndefined, "{} is undefined")                                                                                               \
    M(IterableNextBadReturn, "iterator.next() returned a non-object value")                                                         \
    M(IterableReturnBadReturn, "iterator.return() returned a non-object value")                                                     \
    M(JsonBigInt, "Cannot serialize BigInt value to JSON")                                                                          \
    M(JsonCircular, "Cannot stringify circular object")                                                                             \
    M(JsonMalformed, "Malformed JSON string")                                                                                       \
    M(MathSumPreciseOverflow, "Overflow in Math.sumPrecise")                                                                        \
    M(MissingRequiredProperty, "Required property {} is missing or undefined")                                                      \
    M(ModuleNoEnvironment, "Cannot find module environment for imported binding")                                                   \
    M(ModuleNotFound, "Cannot find/open module: '{}'")                                                                              \
    M(NegativeExponent, "Exponent must be positive")                                                                                \
    M(NoDisposeMethod, "{} does not have dispose method")                                                                           \
    M(NotAConstructor, "{} is not a constructor")                                                                                   \
    M(NotAFunction, "{} is not a function")                                                                                         \
    M(NotAnIntegerOrUndefined, "{} is neither an integer nor undefined")                                                            \
    M(NotAnObject, "{} is not an object")                                                                                           \
    M(NotAnObjectOfType, "Not an object of type {}")                                                                                \
    M(NotAnObjectOrNull, "{} is neither an object nor null")                                                                        \
    M(NotAnObjectOrString, "{} is neither an object nor a string")                                                                  \
    M(NotASharedArrayBuffer, "The array buffer object must be a SharedArrayBuffer")                                                 \
    M(NotAString, "{} is not a string")                                                                                             \
    M(NotASymbol, "{} is not a symbol")                                                                                             \
    M(NotEnoughMemoryToAllocate, "Not enough memory to allocate {} bytes")                                                          \
    M(NotImplemented, "TODO({} is not implemented in LibJS)")                                                                       \
    M(NotIterable, "{} is not iterable")                                                                                            \
    M(NotObjectCoercible, "{} cannot be converted to an object")                                                                    \
    M(NotUndefined, "{} is not undefined")                                                                                          \
    M(NumberIsNaN, "{} must not be NaN")                                                                                            \
    M(NumberIsNaNOrInfinity, "Number must not be NaN or Infinity")                                                                  \
    M(NumberIsNegative, "{} must not be negative")                                                                                  \
    M(ObjectDefineOwnPropertyReturnedFalse, "Object's [[DefineOwnProperty]] method returned false")                                 \
    M(ObjectDeleteReturnedFalse, "Object's [[Delete]] method returned false")                                                       \
    M(ObjectFreezeFailed, "Could not freeze object")                                                                                \
    M(ObjectPreventExtensionsReturnedFalse, "Object's [[PreventExtensions]] method returned false")                                 \
    M(ObjectPrototypeWrongType, "Prototype must be an object or null")                                                              \
    M(ObjectSealFailed, "Could not seal object")                                                                                    \
    M(ObjectSetPrototypeOfReturnedFalse, "Object's [[SetPrototypeOf]] method returned false")                                       \
    M(ObjectSetReturnedFalse, "Object's [[Set]] method returned false")                                                             \
    M(OptionIsNotValidValue, "{} is not a valid value for option {}")                                                               \
    M(OutOfMemory, "Out of memory")                                                                                                 \
    M(OverloadResolutionFailed, "Overload resolution failed")                                                                       \
    M(PrivateFieldAlreadyDeclared, "Private field '{}' has already been declared")                                                  \
    M(PrivateFieldDoesNotExistOnObject, "Private field '{}' does not exist on object")                                              \
    M(PrivateFieldGetAccessorWithoutGetter, "Cannot get private field '{}' as accessor without getter")                             \
    M(PrivateFieldSetAccessorWithoutSetter, "Cannot set private field '{}' as accessor without setter")                             \
    M(PrivateFieldSetMethod, "Cannot set private method '{}'")                                                                      \
    M(PromiseExecutorNotAFunction, "Promise executor must be a function")                                                           \
    M(ProxyConstructBadReturnType, "Proxy handler's construct trap violates invariant: must return an object")                      \
    M(ProxyConstructorBadType, "Expected {} argument of Proxy constructor to be object, got {}")                                    \
    M(ProxyDefinePropExistingConfigurable,                                                                                          \
        "Proxy handler's defineProperty trap violates invariant: a property cannot be defined as non-configurable if it "           \
        "already exists on the target object as a configurable property")                                                           \
    M(ProxyDefinePropIncompatibleDescriptor,                                                                                        \
        "Proxy handler's defineProperty trap violates invariant: the new descriptor is not compatible with the existing "           \
        "descriptor of the property on the target")                                                                                 \
    M(ProxyDefinePropNonConfigurableNonExisting,                                                                                    \
        "Proxy handler's defineProperty trap violates invariant: a property cannot be defined as non-configurable if it "           \
        "does not already exist on the target object")                                                                              \
    M(ProxyDefinePropNonExtensible,                                                                                                 \
        "Proxy handler's defineProperty trap violates invariant: a property cannot be reported as being defined if the "            \
        "property does not exist on the target and the target is non-extensible")                                                   \
    M(ProxyDefinePropNonWritable,                                                                                                   \
        "Proxy handler's defineProperty trap violates invariant: a non-configurable property cannot be non-writable, "              \
        "unless there exists a corresponding non-configurable, non-writable own property of the target object")                     \
    M(ProxyDeleteNonConfigurable,                                                                                                   \
        "Proxy handler's deleteProperty trap violates invariant: cannot report a non-configurable own property of the "             \
        "target as deleted")                                                                                                        \
    M(ProxyDeleteNonExtensible,                                                                                                     \
        "Proxy handler's deleteProperty trap violates invariant: a property cannot be reported as deleted, if it exists "           \
        "as an own property of the target object and the target object is non-extensible. ")                                        \
    M(ProxyGetImmutableDataProperty,                                                                                                \
        "Proxy handler's get trap violates invariant: the returned value must match the value on the target if the "                \
        "property exists on the target as a non-writable, non-configurable own data property")                                      \
    M(ProxyGetNonConfigurableAccessor,                                                                                              \
        "Proxy handler's get trap violates invariant: the returned value must be undefined if the property exists on the "          \
        "target as a non-configurable accessor property with an undefined get attribute")                                           \
    M(ProxyGetOwnDescriptorInvalidDescriptor,                                                                                       \
        "Proxy handler's getOwnPropertyDescriptor trap violates invariant: invalid property descriptor for existing "               \
        "property on the target")                                                                                                   \
    M(ProxyGetOwnDescriptorInvalidNonConfig,                                                                                        \
        "Proxy handler's getOwnPropertyDescriptor trap violates invariant: cannot report target's property as "                     \
        "non-configurable if the property does not exist, or if it is configurable")                                                \
    M(ProxyGetOwnDescriptorNonConfigurable,                                                                                         \
        "Proxy handler's getOwnPropertyDescriptor trap violates invariant: cannot return undefined for a property on the "          \
        "target which is a non-configurable property")                                                                              \
    M(ProxyGetOwnDescriptorNonConfigurableNonWritable,                                                                              \
        "Proxy handler's getOwnPropertyDescriptor trap violates invariant: cannot a property as both non-configurable and "         \
        "non-writable, unless it exists as a non-configurable, non-writable own property of the target object")                     \
    M(ProxyGetOwnDescriptorReturn,                                                                                                  \
        "Proxy handler's getOwnPropertyDescriptor trap violates invariant: must return an object or undefined")                     \
    M(ProxyGetOwnDescriptorUndefinedReturn,                                                                                         \
        "Proxy handler's getOwnPropertyDescriptor trap violates invariant: cannot report a property as being undefined "            \
        "if it exists as an own property of the target and the target is non-extensible")                                           \
    M(ProxyGetPrototypeOfNonExtensible,                                                                                             \
        "Proxy handler's getPrototypeOf trap violates invariant: cannot return a different prototype object for a "                 \
        "non-extensible target")                                                                                                    \
    M(ProxyGetPrototypeOfReturn, "Proxy handler's getPrototypeOf trap violates invariant: must return an object or null")           \
    M(ProxyHasExistingNonConfigurable,                                                                                              \
        "Proxy handler's has trap violates invariant: a property cannot be reported as non-existent if it exists on the "           \
        "target as a non-configurable property")                                                                                    \
    M(ProxyHasExistingNonExtensible,                                                                                                \
        "Proxy handler's has trap violates invariant: a property cannot be reported as non-existent if it exists on the "           \
        "target and the target is non-extensible")                                                                                  \
    M(ProxyIsExtensibleReturn,                                                                                                      \
        "Proxy handler's isExtensible trap violates invariant: return value must match the target's extensibility")                 \
    M(ProxyOwnPropertyKeysDuplicates,                                                                                               \
        "Proxy handler's ownKeys trap violates invariant: the result list may not contain duplicate elements")                      \
    M(ProxyOwnPropertyKeysNonExtensibleNewProperty,                                                                                 \
        "Proxy handler's ownKeys trap violates invariant: cannot report new property '{}' of non-extensible object")                \
    M(ProxyOwnPropertyKeysNonExtensibleSkippedProperty,                                                                             \
        "Proxy handler's ownKeys trap violates invariant: cannot skip property '{}' of non-extensible object")                      \
    M(ProxyOwnPropertyKeysNotStringOrSymbol,                                                                                        \
        "Proxy handler's ownKeys trap violates invariant: the type of each result list element is either String or Symbol")         \
    M(ProxyOwnPropertyKeysSkippedNonconfigurableProperty,                                                                           \
        "Proxy handler's ownKeys trap violates invariant: cannot skip non-configurable property '{}'")                              \
    M(ProxyPreventExtensionsReturn,                                                                                                 \
        "Proxy handler's preventExtensions trap violates invariant: cannot return true if the target object is extensible")         \
    M(ProxyRevoked, "An operation was performed on a revoked Proxy object")                                                         \
    M(ProxySetImmutableDataProperty,                                                                                                \
        "Proxy handler's set trap violates invariant: cannot return true for a property on the target which is a "                  \
        "non-configurable, non-writable own data property")                                                                         \
    M(ProxySetNonConfigurableAccessor,                                                                                              \
        "Proxy handler's set trap violates invariant: cannot return true for a property on the target which is a "                  \
        "non-configurable own accessor property with an undefined set attribute")                                                   \
    M(ProxySetPrototypeOfNonExtensible,                                                                                             \
        "Proxy handler's setPrototypeOf trap violates invariant: the argument must match the prototype of the target if "           \
        "the target is non-extensible")                                                                                             \
    M(ReduceNoInitial, "Reduce of empty array with no initial value")                                                               \
    M(ReferenceNullishDeleteProperty, "Cannot delete property '{}' of {}")                                                          \
    M(ReferenceNullishSetProperty, "Cannot set property '{}' of {}")                                                                \
    M(ReferencePrimitiveSetProperty, "Cannot set property '{}' of {} '{}'")                                                         \
    M(ReferenceUnresolvable, "Unresolvable reference")                                                                              \
    M(RegExpCompileError, "RegExp compile error: {}")                                                                               \
    M(RegExpObjectBadFlag, "Invalid RegExp flag '{}'")                                                                              \
    M(RegExpObjectIncompatibleFlags, "RegExp flag '{}' is incompatible with flag '{}'")                                             \
    M(RegExpObjectRepeatedFlag, "Repeated RegExp flag '{}'")                                                                        \
    M(RestrictedFunctionPropertiesAccess,                                                                                           \
        "Restricted function properties like 'callee', 'caller' and 'arguments' may not be accessed in strict mode")                \
    M(RestrictedGlobalProperty, "Cannot declare global property '{}'")                                                              \
    M(SetLegacyRegExpStaticPropertyThisValueMismatch,                                                                               \
        "Legacy RegExp static property setter must be called with the RegExp constructor for the this value")                       \
    M(ShadowRealmEvaluateAbruptCompletion, "The evaluated script did not complete normally")                                        \
    M(ShadowRealmWrappedValueNonFunctionObject, "Wrapped value must be primitive or a function object, got {}")                     \
    M(SharedArrayBuffer, "The array buffer object cannot be a SharedArrayBuffer")                                                   \
    M(SpeciesConstructorDidNotCreate, "Species constructor did not create {}")                                                      \
    M(SpeciesConstructorReturned, "Species constructor returned {}")                                                                \
    M(StringNonGlobalRegExp, "RegExp argument is non-global")                                                                       \
    M(StringRepeatCountMustBe, "repeat count must be a {} number")                                                                  \
    M(StringRepeatCountMustNotOverflow, "repeat count must not overflow")                                                           \
    M(TemporalDifferentCalendars, "Cannot compare dates from two different calendars")                                              \
    M(TemporalDifferentTimeZones, "Cannot compare dates from two different time zones")                                             \
    M(TemporalDisambiguatePossibleEpochNSRejectMoreThanOne, "Cannot disambiguate two or more possible epoch nanoseconds")           \
    M(TemporalDisambiguatePossibleEpochNSRejectZero, "Cannot disambiguate zero possible epoch nanoseconds")                         \
    M(TemporalInvalidCalendar, "Invalid calendar")                                                                                  \
    M(TemporalInvalidCalendarFieldName, "Invalid calendar field '{}'")                                                              \
    M(TemporalInvalidCalendarIdentifier, "Invalid calendar identifier '{}'")                                                        \
    M(TemporalInvalidCalendarString, "Invalid calendar string '{}'")                                                                \
    M(TemporalInvalidCriticalAnnotation, "Invalid critical annotation: '{}={}'")                                                    \
    M(TemporalInvalidDuration, "Invalid duration")                                                                                  \
    M(TemporalInvalidDurationLikeObject, "Invalid duration-like object")                                                            \
    M(TemporalInvalidDurationPropertyValueNonIntegral, "Invalid value for duration property '{}': must be an integer, got {}")      \
    M(TemporalInvalidDurationString, "Invalid duration string '{}'")                                                                \
    M(TemporalInvalidEpochNanoseconds, "Invalid epoch nanoseconds value, must be in range -86400 * 10^17 to 86400 * 10^17")         \
    M(TemporalInvalidInstantString, "Invalid instant string '{}'")                                                                  \
    M(TemporalInvalidISODate, "Invalid ISO date")                                                                                   \
    M(TemporalInvalidISODateTime, "Invalid ISO date time")                                                                          \
    M(TemporalInvalidLargestUnit, "Largest unit must not be {}")                                                                    \
    M(TemporalInvalidMonthCode, "Invalid month code")                                                                               \
    M(TemporalInvalidPlainDate, "Invalid plain date")                                                                               \
    M(TemporalInvalidPlainDateTime, "Invalid plain date time")                                                                      \
    M(TemporalInvalidPlainMonthDay, "Invalid plain month day")                                                                      \
    M(TemporalInvalidPlainTime, "Invalid plain time")                                                                               \
    M(TemporalInvalidPlainYearMonth, "Invalid plain year month")                                                                    \
    M(TemporalInvalidTime, "Invalid time")                                                                                          \
    M(TemporalInvalidTimeLikeField, "Invalid value {} for time field '{}'")                                                         \
    M(TemporalInvalidTimeZoneName, "Invalid time zone name '{}'")                                                                   \
    M(TemporalInvalidTimeZoneString, "Invalid time zone string '{}'")                                                               \
    M(TemporalInvalidUnitRange, "Invalid unit range, {} is larger than {}")                                                         \
    M(TemporalInvalidZonedDateTimeOffset, "Invalid offset for the provided date and time in the current time zone")                 \
    M(TemporalInvalidZonedDateTimeString, "Invalid zoned date time string '{}'")                                                    \
    M(TemporalMissingOptionsObject, "Required options object is missing or undefined")                                              \
    M(TemporalMissingStartingPoint, "A starting point is required for comparing {}")                                                \
    M(TemporalMissingUnits, "One or both of smallestUnit or largestUnit is required")                                               \
    M(TemporalObjectMustBePartialTemporalObject, "Object must be a partial Temporal object")                                        \
    M(ThisHasNotBeenInitialized, "|this| has not been initialized")                                                                 \
    M(ThisIsAlreadyInitialized, "|this| is already initialized")                                                                    \
    M(ToObjectNullOrUndefined, "ToObject on null or undefined")                                                                     \
    M(ToObjectNullOrUndefinedWithName, "\"{}\" is {}")                                                                              \
    M(ToObjectNullOrUndefinedWithProperty, "Cannot access property \"{}\" on {} object")                                            \
    M(ToObjectNullOrUndefinedWithPropertyAndName, "Cannot access property \"{}\" on {} object \"{}\"")                              \
    M(TopLevelVariableAlreadyDeclared, "Redeclaration of top level variable '{}'")                                                  \
    M(ToPrimitiveReturnedObject, "Can't convert {} to primitive with hint \"{}\", its @@toPrimitive method returned an object")     \
    M(TypedArrayContentTypeMismatch, "Can't create {} from {}")                                                                     \
    M(TypedArrayInvalidBufferLength, "Invalid buffer length for {}: must be a multiple of {}, got {}")                              \
    M(TypedArrayInvalidByteOffset, "Invalid byte offset for {}: must be a multiple of {}, got {}")                                  \
    M(TypedArrayInvalidCopy, "Copy between arrays of different content types ({} and {}) is prohibited")                            \
    M(TypedArrayInvalidIntegerIndex, "Invalid integer index: {}")                                                                   \
    M(TypedArrayInvalidTargetOffset, "Invalid target offset: must be {}")                                                           \
    M(TypedArrayOutOfRangeByteOffset, "Typed array byte offset {} is out of range for buffer with length {}")                       \
    M(TypedArrayOutOfRangeByteOffsetOrLength, "Typed array range {}:{} is out of range for buffer with length {}")                  \
    M(TypedArrayOverflow, "Overflow in {}")                                                                                         \
    M(TypedArrayOverflowOrOutOfBounds, "Overflow or out of bounds in {}")                                                           \
    M(TypedArrayPrototypeOneArg, "TypedArray.prototype.{}() requires at least one argument")                                        \
    M(TypedArrayTypeIsNot, "Typed array {} element type is not {}")                                                                 \
    M(UnknownIdentifier, "'{}' is not defined")                                                                                     \
    M(UnsupportedDeleteSuperProperty, "Can't delete a property on 'super'")                                                         \
    M(URIMalformed, "URI malformed") /* LibWeb bindings */                                                                          \
    M(WrappedFunctionCallThrowCompletion, "Call of wrapped target function did not complete normally")                              \
    M(WrappedFunctionCopyNameAndLengthThrowCompletion, "Trying to copy target name and length did not complete normally")           \
    M(YieldFromIteratorMissingThrowMethod, "yield* protocol violation: iterator must have a throw method")

namespace JS {

class ErrorType {
public:
#define __ENUMERATE_JS_ERROR(name, message) \
    static const ErrorType name;
    JS_ENUMERATE_ERROR_TYPES(__ENUMERATE_JS_ERROR)
#undef __ENUMERATE_JS_ERROR

    String message() const
    {
        return m_message;
    }

private:
    explicit ErrorType(StringView message)
        : m_message(MUST(String::from_utf8(message)))
    {
    }

    String m_message;
};

}
