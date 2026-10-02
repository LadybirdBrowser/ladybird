/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::fmt;

use ak::Utf16String;

use crate::runtime::value::number_to_string;
use crate::utf16::{Utf16Display, Utf16StringBuilder, utf16_formatted};

macro_rules! define_error_types {
    ($($name:ident => $format:literal,)*) => {
        /// The messages of the errors the runtime throws, as in Libraries/LibJS/Runtime/ErrorTypes.h. Each `{}` in a
        /// format is replaced by the next argument.
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        pub enum ErrorType {
            $($name,)*
        }

        impl ErrorType {
            pub const fn format(self) -> &'static str {
                match self {
                    $(Self::$name => $format,)*
                }
            }
        }
    };
}

#[rustfmt::skip]
define_error_types! {
    AccessorBadField => "Accessor descriptor's '{}' field must be a function or undefined",
    AccessorValueOrWritable => "Accessor property descriptor cannot specify a value or writable key",
    AgentCannotSuspend => "Agent is not allowed to suspend",
    ArrayMaxSize => "Maximum array size exceeded",
    AsyncDisposableStackAlreadyDisposed => "AsyncDisposableStack is already disposed",
    BadArgCountMany => "{}() needs {} arguments",
    BadArgCountOne => "{}() needs one argument",
    BigIntBadOperator => "Cannot use {} operator with BigInt",
    BigIntBadOperatorOtherType => "Cannot use {} operator with BigInt and other type",
    BigIntFromNonIntegral => "Cannot convert non-integral number to BigInt",
    BigIntInvalidValue => "Invalid value for BigInt: {}",
    BigIntSizeExceeded => "Maximum BigInt size exceeded",
    BindingNotInitialized => "Binding {} is not initialized",
    BufferOutOfBounds => "{} contains a property which references a value at an index not contained within its buffer's bounds",
    ByteLengthExceedsMaxByteLength => "ArrayBuffer byte length of {} exceeds the max byte length of {}",
    ByteLengthLessThanPreviousByteLength => "SharedArrayBuffer byte length of {} is less than the previous byte length of {}",
    CallStackSizeExceeded => "Call stack size limit exceeded",
    CannotBeHeldWeakly => "{} cannot be held weakly",
    CannotDeclareGlobalFunction => "Cannot declare global function of name '{}'",
    CannotDeclareGlobalVariable => "Cannot declare global variable of name '{}'",
    ClassConstructorWithoutNew => "Class constructor {} must be called with 'new'",
    ClassExtendsValueInvalidPrototype => "Class extends value has an invalid prototype {}",
    ClassExtendsValueNotAConstructorOrNull => "Class extends value {} is not a constructor or null",
    ClassIsAbstract => "Abstract class {} cannot be constructed directly",
    ConstructorWithoutNew => "{} constructor must be called with 'new'",
    Convert => "Cannot convert {} to {}",
    DataViewOutOfRangeByteOffset => "Data view byte offset {} is out of range for buffer with length {}",
    DerivedConstructorReturningInvalidValue => "Derived constructor return invalid value",
    DescWriteNonWritable => "Cannot write to non-writable property '{}'",
    DetachedArrayBuffer => "ArrayBuffer is detached",
    DetachKeyMismatch => "Provided detach key {} does not match the ArrayBuffer's detach key {}",
    DisposableStackAlreadyDisposed => "DisposableStack already disposed values",
    DivisionByZero => "Division by zero",
    DynamicImportNotAllowed => "Dynamic Imports are not allowed",
    FinalizationRegistrySameTargetAndValue => "Target and held value must not be the same",
    FixedArrayBuffer => "ArrayBuffer is not resizable",
    GeneratorAlreadyExecuting => "Generator is already executing",
    GeneratorBrandMismatch => "Generator brand '{}' does not match generator brand '{}')",
    GetCapabilitiesExecutorCalledMultipleTimes => "GetCapabilitiesExecutor was called multiple times",
    GetLegacyRegExpStaticPropertyThisValueMismatch => "Legacy RegExp static property getter must be called with the RegExp constructor for the this value",
    GetLegacyRegExpStaticPropertyValueEmpty => "Legacy RegExp static property getter value is empty",
    GlobalEnvironmentAlreadyHasBinding => "Global environment already has binding '{}'",
    ImportAttributeUnsupported => "Every import attribute is not supported",
    IndexOutOfRange => "Index {} is out of range of array length {}",
    InOperatorWithObject => "'in' operator must be used on an object",
    InstanceOfOperatorBadPrototype => "'prototype' property of {} is not an object",
    IntlFractionalUnitFollowedByNonFractionalUnit => "Non-fractional unit {} is not allowed after a fractional unit",
    IntlFractionalUnitsMixedWithAlwaysDisplay => "Fractional unit {} may not be used with {} value of 'always'",
    IntlInvalidDateTimeFormatOption => "Option {} cannot be set when also providing {}",
    IntlInvalidKey => "{} is not a valid key",
    IntlInvalidLanguageTag => "{} is not a structurally valid language tag",
    IntlInvalidRoundingIncrement => "{} is not a valid rounding increment",
    IntlInvalidRoundingIncrementForFractionDigits => "{} is not a valid rounding increment for inequal min/max fraction digits",
    IntlInvalidRoundingIncrementForRoundingType => "{} is not a valid rounding increment for rounding type {}",
    IntlInvalidTime => "Time value must be between -8.64E15 and 8.64E15",
    IntlInvalidUnit => "Unit {} is not a valid time unit",
    IntlMinimumExceedsMaximum => "Minimum value {} is larger than maximum value {}",
    IntlNonNumericOr2DigitAfterNumericOr2Digit => "Styles other than 'fractional', numeric', or '2-digit' may not be used in smaller units after being used in larger units",
    IntlNumberIsNaNOrOutOfRange => "Value {} is NaN or is not between {} and {}",
    IntlOptionUndefined => "Option {} must be defined when option {} is {}",
    IntlTemporalFormatIsNull => "Unable to determine format for {}",
    IntlTemporalFormatRangeTypeMismatch => "Cannot format a date-time range with different date-time types",
    IntlTemporalInvalidCalendar => "Cannot format {} with calendar '{}' in locale with calendar '{}'",
    IntlTemporalZonedDateTime => "Cannot format Temporal.ZonedDateTime, use Temporal.ZonedDateTime.prototype.toLocaleString",
    IntlUnsupportedLanguageTag => "{} is not a supported language tag",
    InvalidAssignToConst => "Invalid assignment to const variable",
    InvalidCodePoint => "Invalid code point {}, must be an integer no less than 0 and no greater than 0x10FFFF",
    InvalidEnumerationValue => "Invalid value '{}' for enumeration type '{}'",
    InvalidFractionDigits => "Fraction Digits must be an integer no less than 0, and no greater than 100",
    InvalidHint => "Invalid hint: \"{}\"",
    InvalidIndex => "Index must be a positive integer no greater than 2^53-1",
    InvalidLeftHandAssignment => "Invalid left-hand side in assignment",
    InvalidLength => "Invalid {} length",
    InvalidNormalizationForm => "The normalization form must be one of NFC, NFD, NFKC, NFKD. Got '{}'",
    InvalidOrAmbiguousExportEntry => "Invalid or ambiguous export entry '{}'",
    InvalidPrecision => "Precision must be an integer no less than 1, and no greater than 100",
    InvalidRadix => "Radix must be an integer no less than 2, and no greater than 36",
    InvalidRestrictedFloatingPointParameter => "Expected {} to be a finite floating-point number",
    InvalidTimeValue => "Invalid time value",
    IsNotA => "{} is not a {}",
    IsNotAEvaluatedFrom => "{} is not a {} (evaluated from '{}')",
    IsNotAn => "{} is not an {}",
    IsUndefined => "{} is undefined",
    IterableNextBadReturn => "iterator.next() returned a non-object value",
    IterableReturnBadReturn => "iterator.return() returned a non-object value",
    JsonBigInt => "Cannot serialize BigInt value to JSON",
    JsonCircular => "Cannot stringify circular object",
    JsonMalformed => "Malformed JSON string",
    JsonRawJSONNonPrimitive => "JSON.rawJSON cannot accept object or array as outermost value",
    MathSumPreciseOverflow => "Overflow in Math.sumPrecise",
    MissingRequiredProperty => "Required property {} is missing or undefined",
    ModuleNoEnvironment => "Cannot find module environment for imported binding",
    ModuleNotFound => "Cannot find/open module: '{}'",
    NegativeExponent => "Exponent must be positive",
    NoDisposeMethod => "{} does not have dispose method",
    NotAConstructor => "{} is not a constructor",
    NotAFunction => "{} is not a function",
    NotAnIntegerOrUndefined => "{} is neither an integer nor undefined",
    NotAnObject => "{} is not an object",
    NotAnObjectOfType => "Not an object of type {}",
    NotAnObjectOrNull => "{} is neither an object nor null",
    NotAnObjectOrString => "{} is neither an object nor a string",
    NotASharedArrayBuffer => "The array buffer object must be a SharedArrayBuffer",
    NotAString => "{} is not a string",
    NotASymbol => "{} is not a symbol",
    NotEnoughMemoryToAllocate => "Not enough memory to allocate {} bytes",
    NotImplemented => "TODO({} is not implemented in LibJS)",
    NotIterable => "{} is not iterable",
    NotObjectCoercible => "{} cannot be converted to an object",
    NotUndefined => "{} is not undefined",
    NumberIsLargerThanMaxSafeNumber => "{} must be less than 2^53",
    NumberIsNaN => "{} must not be NaN",
    NumberIsNaNOrInfinity => "Number must not be NaN or Infinity",
    NumberIsNegative => "{} must not be negative",
    ObjectDefineOwnPropertyReturnedFalse => "Object's [[DefineOwnProperty]] method returned false",
    ObjectDeleteReturnedFalse => "Object's [[Delete]] method returned false",
    ObjectFreezeFailed => "Could not freeze object",
    ObjectPreventExtensionsReturnedFalse => "Object's [[PreventExtensions]] method returned false",
    ObjectPrototypeWrongType => "Prototype must be an object or null",
    ObjectSealFailed => "Could not seal object",
    ObjectSetPrototypeOfReturnedFalse => "Object's [[SetPrototypeOf]] method returned false",
    ObjectSetReturnedFalse => "Object's [[Set]] method returned false",
    OptionIsNotValidValue => "{} is not a valid value for option {}",
    OutOfMemory => "Out of memory",
    OverloadResolutionFailed => "Overload resolution failed",
    PrivateFieldAlreadyDeclared => "Private field '{}' has already been declared",
    PrivateFieldNotDeclared => "Reference to undeclared private field or method '{}'",
    PrivateFieldDoesNotExistOnObject => "Private field '{}' does not exist on object",
    PrivateFieldGetAccessorWithoutGetter => "Cannot get private field '{}' as accessor without getter",
    PrivateFieldSetAccessorWithoutSetter => "Cannot set private field '{}' as accessor without setter",
    PrivateFieldSetMethod => "Cannot set private method '{}'",
    PromiseExecutorNotAFunction => "Promise executor must be a function",
    ProxyConstructBadReturnType => "Proxy handler's construct trap violates invariant: must return an object",
    ProxyConstructorBadType => "Expected {} argument of Proxy constructor to be object, got {}",
    ProxyDefinePropExistingConfigurable => "Proxy handler's defineProperty trap violates invariant: a property cannot be defined as non-configurable if it already exists on the target object as a configurable property",
    ProxyDefinePropIncompatibleDescriptor => "Proxy handler's defineProperty trap violates invariant: the new descriptor is not compatible with the existing descriptor of the property on the target",
    ProxyDefinePropNonConfigurableNonExisting => "Proxy handler's defineProperty trap violates invariant: a property cannot be defined as non-configurable if it does not already exist on the target object",
    ProxyDefinePropNonExtensible => "Proxy handler's defineProperty trap violates invariant: a property cannot be reported as being defined if the property does not exist on the target and the target is non-extensible",
    ProxyDefinePropNonWritable => "Proxy handler's defineProperty trap violates invariant: a non-configurable property cannot be non-writable, unless there exists a corresponding non-configurable, non-writable own property of the target object",
    ProxyDeleteNonConfigurable => "Proxy handler's deleteProperty trap violates invariant: cannot report a non-configurable own property of the target as deleted",
    ProxyDeleteNonExtensible => "Proxy handler's deleteProperty trap violates invariant: a property cannot be reported as deleted, if it exists as an own property of the target object and the target object is non-extensible. ",
    ProxyGetImmutableDataProperty => "Proxy handler's get trap violates invariant: the returned value must match the value on the target if the property exists on the target as a non-writable, non-configurable own data property",
    ProxyGetNonConfigurableAccessor => "Proxy handler's get trap violates invariant: the returned value must be undefined if the property exists on the target as a non-configurable accessor property with an undefined get attribute",
    ProxyGetOwnDescriptorInvalidDescriptor => "Proxy handler's getOwnPropertyDescriptor trap violates invariant: invalid property descriptor for existing property on the target",
    ProxyGetOwnDescriptorInvalidNonConfig => "Proxy handler's getOwnPropertyDescriptor trap violates invariant: cannot report target's property as non-configurable if the property does not exist, or if it is configurable",
    ProxyGetOwnDescriptorNonConfigurable => "Proxy handler's getOwnPropertyDescriptor trap violates invariant: cannot return undefined for a property on the target which is a non-configurable property",
    ProxyGetOwnDescriptorNonConfigurableNonWritable => "Proxy handler's getOwnPropertyDescriptor trap violates invariant: cannot a property as both non-configurable and non-writable, unless it exists as a non-configurable, non-writable own property of the target object",
    ProxyGetOwnDescriptorReturn => "Proxy handler's getOwnPropertyDescriptor trap violates invariant: must return an object or undefined",
    ProxyGetOwnDescriptorUndefinedReturn => "Proxy handler's getOwnPropertyDescriptor trap violates invariant: cannot report a property as being undefined if it exists as an own property of the target and the target is non-extensible",
    ProxyGetPrototypeOfNonExtensible => "Proxy handler's getPrototypeOf trap violates invariant: cannot return a different prototype object for a non-extensible target",
    ProxyGetPrototypeOfReturn => "Proxy handler's getPrototypeOf trap violates invariant: must return an object or null",
    ProxyHasExistingNonConfigurable => "Proxy handler's has trap violates invariant: a property cannot be reported as non-existent if it exists on the target as a non-configurable property",
    ProxyHasExistingNonExtensible => "Proxy handler's has trap violates invariant: a property cannot be reported as non-existent if it exists on the target and the target is non-extensible",
    ProxyIsExtensibleReturn => "Proxy handler's isExtensible trap violates invariant: return value must match the target's extensibility",
    ProxyOwnPropertyKeysDuplicates => "Proxy handler's ownKeys trap violates invariant: the result list may not contain duplicate elements",
    ProxyOwnPropertyKeysNonExtensibleNewProperty => "Proxy handler's ownKeys trap violates invariant: cannot report new property '{}' of non-extensible object",
    ProxyOwnPropertyKeysNonExtensibleSkippedProperty => "Proxy handler's ownKeys trap violates invariant: cannot skip property '{}' of non-extensible object",
    ProxyOwnPropertyKeysNotStringOrSymbol => "Proxy handler's ownKeys trap violates invariant: the type of each result list element is either String or Symbol",
    ProxyOwnPropertyKeysSkippedNonconfigurableProperty => "Proxy handler's ownKeys trap violates invariant: cannot skip non-configurable property '{}'",
    ProxyPreventExtensionsReturn => "Proxy handler's preventExtensions trap violates invariant: cannot return true if the target object is extensible",
    ProxyRevoked => "An operation was performed on a revoked Proxy object",
    ProxySetImmutableDataProperty => "Proxy handler's set trap violates invariant: cannot return true for a property on the target which is a non-configurable, non-writable own data property",
    ProxySetNonConfigurableAccessor => "Proxy handler's set trap violates invariant: cannot return true for a property on the target which is a non-configurable own accessor property with an undefined set attribute",
    ProxySetPrototypeOfNonExtensible => "Proxy handler's setPrototypeOf trap violates invariant: the argument must match the prototype of the target if the target is non-extensible",
    ReduceNoInitial => "Reduce of empty array with no initial value",
    ReferenceNullishDeleteProperty => "Cannot delete property '{}' of {}",
    ReferenceNullishSetProperty => "Cannot set property '{}' of {}",
    ReferencePrimitiveSetProperty => "Cannot set property '{}' of {} '{}'",
    ReferenceUnresolvable => "Unresolvable reference",
    RegExpBacktrackLimitExceeded => "Regular expression backtrack limit exceeded",
    RegExpCompileError => "RegExp compile error: {}",
    RegExpObjectBadFlag => "Invalid RegExp flag '{}'",
    RegExpObjectIncompatibleFlags => "RegExp flag '{}' is incompatible with flag '{}'",
    RegExpObjectRepeatedFlag => "Repeated RegExp flag '{}'",
    RestrictedFunctionPropertiesAccess => "Restricted function properties like 'callee', 'caller' and 'arguments' may not be accessed in strict mode",
    RestrictedGlobalProperty => "Cannot declare global property '{}'",
    SetLegacyRegExpStaticPropertyThisValueMismatch => "Legacy RegExp static property setter must be called with the RegExp constructor for the this value",
    SharedArrayBuffer => "The array buffer object cannot be a SharedArrayBuffer",
    SpeciesConstructorDidNotCreate => "Species constructor did not create {}",
    SpeciesConstructorReturned => "Species constructor returned {}",
    StringNonGlobalRegExp => "RegExp argument is non-global",
    StringRepeatCountMustBe => "repeat count must be a {} number",
    StringRepeatCountMustNotOverflow => "repeat count must not overflow",
    StringSizeMustNotOverflow => "string size must not overflow",
    TemporalDifferentCalendars => "Cannot compare dates from two different calendars",
    TemporalDifferentTimeZones => "Cannot compare dates from two different time zones",
    TemporalDisambiguatePossibleEpochNSRejectMoreThanOne => "Cannot disambiguate two or more possible epoch nanoseconds",
    TemporalDisambiguatePossibleEpochNSRejectZero => "Cannot disambiguate zero possible epoch nanoseconds",
    TemporalInvalidCalendar => "Invalid calendar",
    TemporalInvalidCalendarFieldName => "Invalid calendar field '{}'",
    TemporalInvalidCalendarIdentifier => "Invalid calendar identifier '{}'",
    TemporalInvalidCalendarString => "Invalid calendar string '{}'",
    TemporalInvalidCriticalAnnotation => "Invalid critical annotation: '{}={}'",
    TemporalInvalidDuration => "Invalid duration",
    TemporalInvalidDurationLikeObject => "Invalid duration-like object",
    TemporalInvalidDurationPropertyValueNonIntegral => "Invalid value for duration property '{}': must be an integer, got {}",
    TemporalInvalidDurationString => "Invalid duration string '{}'",
    TemporalInvalidEpochNanoseconds => "Invalid epoch nanoseconds value, must be in range -86400 * 10^17 to 86400 * 10^17",
    TemporalInvalidInstantString => "Invalid instant string '{}'",
    TemporalInvalidISODate => "Invalid ISO date",
    TemporalInvalidISODateTime => "Invalid ISO date time",
    TemporalInvalidLargestUnit => "Largest unit must not be {}",
    TemporalInvalidMonthCode => "Invalid month code",
    TemporalInvalidPlainDate => "Invalid plain date",
    TemporalInvalidPlainDateTime => "Invalid plain date time",
    TemporalInvalidPlainMonthDay => "Invalid plain month day",
    TemporalInvalidPlainTime => "Invalid plain time",
    TemporalInvalidPlainYearMonth => "Invalid plain year month",
    TemporalInvalidPlainYearMonthAddition => "Only years and months may be {} Temporal.PlainYearMonth",
    TemporalInvalidTime => "Invalid time",
    TemporalInvalidTimeLikeField => "Invalid value {} for time field '{}'",
    TemporalInvalidTimeZoneName => "Invalid time zone name '{}'",
    TemporalInvalidTimeZoneString => "Invalid time zone string '{}'",
    TemporalInvalidUnitRange => "Invalid unit range, {} is larger than {}",
    TemporalInvalidZonedDateTimeOffset => "Invalid offset for the provided date and time in the current time zone",
    TemporalInvalidZonedDateTimeString => "Invalid zoned date time string '{}'",
    TemporalMissingOptionsObject => "Required options object is missing or undefined",
    TemporalMissingStartingPoint => "A starting point is required for comparing {}",
    TemporalMissingUnits => "One or both of smallestUnit or largestUnit is required",
    TemporalObjectMustBePartialTemporalObject => "Object must be a partial Temporal object",
    ThisHasNotBeenInitialized => "|this| has not been initialized",
    ThisIsAlreadyInitialized => "|this| is already initialized",
    ToObjectNullOrUndefined => "ToObject on null or undefined",
    ToObjectNullOrUndefinedWithName => "\"{}\" is {}",
    ToObjectNullOrUndefinedWithProperty => "Cannot access property \"{}\" on {} object",
    ToObjectNullOrUndefinedWithPropertyAndName => "Cannot access property \"{}\" on {} object \"{}\"",
    TopLevelVariableAlreadyDeclared => "Redeclaration of top level variable '{}'",
    EvalVarHoistingConflict => "Cannot declare var '{}': there is already a lexical declaration with that name in scope",
    ToPrimitiveReturnedObject => "Can't convert {} to primitive with hint \"{}\", its @@toPrimitive method returned an object",
    TypedArrayContentTypeMismatch => "Can't create {} from {}",
    TypedArrayInvalidBufferLength => "Invalid buffer length for {}: must be a multiple of {}, got {}",
    TypedArrayInvalidByteOffset => "Invalid byte offset for {}: must be a multiple of {}, got {}",
    TypedArrayInvalidCopy => "Copy between arrays of different content types ({} and {}) is prohibited",
    TypedArrayInvalidIntegerIndex => "Invalid integer index: {}",
    TypedArrayInvalidTargetOffset => "Invalid target offset: must be {}",
    TypedArrayOutOfRangeByteOffset => "Typed array byte offset {} is out of range for buffer with length {}",
    TypedArrayOutOfRangeByteOffsetOrLength => "Typed array range {}:{} is out of range for buffer with length {}",
    TypedArrayOverflow => "Overflow in {}",
    TypedArrayOverflowOrOutOfBounds => "Overflow or out of bounds in {}",
    TypedArrayPrototypeOneArg => "TypedArray.prototype.{}() requires at least one argument",
    TypedArrayTypeIsNot => "Typed array {} element type is not {}",
    UnknownIdentifier => "'{}' is not defined",
    UnsupportedDeleteSuperProperty => "Can't delete a property on 'super'",
    URIMalformed => "URI malformed",
    WrappedFunctionCallThrowCompletion => "Call of wrapped target function did not complete normally",
    WrappedFunctionCopyNameAndLengthThrowCompletion => "Trying to copy target name and length did not complete normally",
    YieldFromIteratorMissingThrowMethod => "yield* protocol violation: iterator must have a throw method",
    ZipIteratorNotEnoughResults => "Not enough iterator results in 'strict' mode",
}

impl ErrorType {
    /// The message with each `{}` replaced by the next of `arguments`, as AK's Utf16String::formatted() does.
    pub fn message(self, arguments: &[&dyn Utf16Display]) -> Utf16String {
        utf16_formatted(self.format(), arguments)
    }
}

/// A double as AK's Formatter<double> formats it into a message: the shortest digits that round-trip, laid out like
/// Number::toString, except that zeros are "0" and the non-finite values are "nan", "inf" and "-inf".
pub struct AkDouble(pub f64);

impl fmt::Display for AkDouble {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let value = self.0;
        if value.is_nan() {
            return formatter.write_str("nan");
        }
        if value.is_infinite() {
            return formatter.write_str(if value < 0.0 { "-inf" } else { "inf" });
        }
        formatter.write_str(&number_to_string(value))
    }
}

impl Utf16Display for AkDouble {
    fn fmt_utf16(&self, builder: &mut Utf16StringBuilder) {
        builder.append_utf8(&self.to_string());
    }
}
