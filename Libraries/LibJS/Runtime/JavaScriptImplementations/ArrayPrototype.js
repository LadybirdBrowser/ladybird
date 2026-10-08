/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// NB: Property keys of array indices are the numbers themselves here: `k in object` and `object[k]` convert k with
//     ToPropertyKey, which is ! ToString(𝔽(k)) for these integers.

/**
 * 23.1.3.6 Array.prototype.every ( callback [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.every
 */
function every(callback, thisArg) {
    // 1. Let obj be ? ToObject(this value).
    const object = ToObject(this);

    // 2. Let length be ? LengthOfArrayLike(obj).
    const length = ToLength(object.length);

    // 3. If IsCallable(callback) is false, throw a TypeError exception.
    if (!IsCallable(callback)) ThrowNotAFunction(callback);

    // 4. Let k be 0.
    // 5. Repeat, while k < length,
    for (let k = 0; k < length; ++k) {
        // a. Let propertyKey be ! ToString(𝔽(k)).
        // b. Let kPresent be ? HasProperty(obj, propertyKey).
        // c. If kPresent is true, then
        if (k in object) {
            // i. Let kValue be ? Get(obj, propertyKey).
            const kValue = object[k];

            // ii. Let testResult be ToBoolean(? Call(callback, thisArg, « kValue, 𝔽(k), obj »)).
            // iii. If testResult is false, return false.
            if (!Call(callback, thisArg, kValue, k, object)) return false;
        }

        // d. Set k to k + 1.
    }

    // 6. Return true.
    return true;
}

/**
 * 23.1.3.8 Array.prototype.filter ( callback [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.filter
 */
function filter(callback, thisArg) {
    // 1. Let obj be ? ToObject(this value).
    const object = ToObject(this);

    // 2. Let length be ? LengthOfArrayLike(obj).
    const length = ToLength(object.length);

    // 3. If IsCallable(callback) is false, throw a TypeError exception.
    if (!IsCallable(callback)) ThrowNotAFunction(callback);

    // 4. Let array be ? ArraySpeciesCreate(obj, 0).
    const array = ArraySpeciesCreate(object, 0);

    // 5. Let k be 0.
    // 6. Let to be 0.
    let to = 0;

    // 7. Repeat, while k < length,
    for (let k = 0; k < length; ++k) {
        // a. Let propertyKey be ! ToString(𝔽(k)).
        // b. Let kPresent be ? HasProperty(obj, propertyKey).
        // c. If kPresent is true, then
        if (k in object) {
            // i. Let kValue be ? Get(obj, propertyKey).
            const kValue = object[k];

            // ii. Let selected be ToBoolean(? Call(callback, thisArg, « kValue, 𝔽(k), obj »)).
            // iii. If selected is true, then
            if (Call(callback, thisArg, kValue, k, object)) {
                // 1. Perform ? CreateDataPropertyOrThrow(array, ! ToString(𝔽(to)), kValue).
                CreateDataPropertyOrThrow(array, to, kValue);

                // 2. Set to to to + 1.
                ++to;
            }
        }

        // d. Set k to k + 1.
    }

    // 8. Return array.
    return array;
}

/**
 * 23.1.3.9 Array.prototype.find ( predicate [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.find
 */
function find(predicate, thisArg) {
    // 1. Let obj be ? ToObject(this value).
    const object = ToObject(this);

    // 2. Let length be ? LengthOfArrayLike(obj).
    const length = ToLength(object.length);

    // 3. Let findRecord be ? FindViaPredicate(obj, length, ascending, predicate, thisArg).
    // 4. Return findRecord.[[Value]].
    // NB: FindViaPredicate's steps follow, with its records returned as the value they are read for.

    // 1. If IsCallable(predicate) is false, throw a TypeError exception.
    if (!IsCallable(predicate)) ThrowNotAFunction(predicate);

    // 2. If direction is ascending, then
    //    a. Let indices be a List of the integers in the interval from 0 (inclusive) to length (exclusive), in
    //       ascending order.
    // 4. For each integer k of indices, do
    for (let k = 0; k < length; ++k) {
        // a. Let propertyKey be ! ToString(𝔽(k)).
        // b. NOTE: If obj is a TypedArray, the following invocation of Get will return a normal completion.
        // c. Let kValue be ? Get(obj, propertyKey).
        const kValue = object[k];

        // d. Let testResult be ? Call(predicate, thisArg, « kValue, 𝔽(k), obj »).
        // e. If ToBoolean(testResult) is true, return the Record { [[Index]]: 𝔽(k), [[Value]]: kValue }.
        if (Call(predicate, thisArg, kValue, k, object)) return kValue;
    }

    // 5. Return the Record { [[Index]]: -1𝔽, [[Value]]: undefined }.
    return undefined;
}

/**
 * 23.1.3.10 Array.prototype.findIndex ( predicate [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.findindex
 */
function findIndex(predicate, thisArg) {
    // 1. Let obj be ? ToObject(this value).
    const object = ToObject(this);

    // 2. Let length be ? LengthOfArrayLike(obj).
    const length = ToLength(object.length);

    // 3. Let findRecord be ? FindViaPredicate(obj, length, ascending, predicate, thisArg).
    // 4. Return findRecord.[[Index]].
    // NB: FindViaPredicate's steps follow, with its records returned as the value they are read for.

    // 1. If IsCallable(predicate) is false, throw a TypeError exception.
    if (!IsCallable(predicate)) ThrowNotAFunction(predicate);

    // 2. If direction is ascending, then
    //    a. Let indices be a List of the integers in the interval from 0 (inclusive) to length (exclusive), in
    //       ascending order.
    // 4. For each integer k of indices, do
    for (let k = 0; k < length; ++k) {
        // a. Let propertyKey be ! ToString(𝔽(k)).
        // b. NOTE: If obj is a TypedArray, the following invocation of Get will return a normal completion.
        // c. Let kValue be ? Get(obj, propertyKey).
        const kValue = object[k];

        // d. Let testResult be ? Call(predicate, thisArg, « kValue, 𝔽(k), obj »).
        // e. If ToBoolean(testResult) is true, return the Record { [[Index]]: 𝔽(k), [[Value]]: kValue }.
        if (Call(predicate, thisArg, kValue, k, object)) return k;
    }

    // 5. Return the Record { [[Index]]: -1𝔽, [[Value]]: undefined }.
    return -1;
}

/**
 * 23.1.3.11 Array.prototype.findLast ( predicate [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.findlast
 */
function findLast(predicate, thisArg) {
    // 1. Let obj be ? ToObject(this value).
    const object = ToObject(this);

    // 2. Let length be ? LengthOfArrayLike(obj).
    const length = ToLength(object.length);

    // 3. Let findRecord be ? FindViaPredicate(obj, length, descending, predicate, thisArg).
    // 4. Return findRecord.[[Value]].
    // NB: FindViaPredicate's steps follow, with its records returned as the value they are read for.

    // 1. If IsCallable(predicate) is false, throw a TypeError exception.
    if (!IsCallable(predicate)) ThrowNotAFunction(predicate);

    // 3. Else,
    //    a. Let indices be a List of the integers in the interval from 0 (inclusive) to length (exclusive), in
    //       descending order.
    // 4. For each integer k of indices, do
    for (let k = length - 1; k >= 0; --k) {
        // a. Let propertyKey be ! ToString(𝔽(k)).
        // b. NOTE: If obj is a TypedArray, the following invocation of Get will return a normal completion.
        // c. Let kValue be ? Get(obj, propertyKey).
        const kValue = object[k];

        // d. Let testResult be ? Call(predicate, thisArg, « kValue, 𝔽(k), obj »).
        // e. If ToBoolean(testResult) is true, return the Record { [[Index]]: 𝔽(k), [[Value]]: kValue }.
        if (Call(predicate, thisArg, kValue, k, object)) return kValue;
    }

    // 5. Return the Record { [[Index]]: -1𝔽, [[Value]]: undefined }.
    return undefined;
}

/**
 * 23.1.3.12 Array.prototype.findLastIndex ( predicate [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.findlastindex
 */
function findLastIndex(predicate, thisArg) {
    // 1. Let obj be ? ToObject(this value).
    const object = ToObject(this);

    // 2. Let length be ? LengthOfArrayLike(obj).
    const length = ToLength(object.length);

    // 3. Let findRecord be ? FindViaPredicate(obj, length, descending, predicate, thisArg).
    // 4. Return findRecord.[[Index]].
    // NB: FindViaPredicate's steps follow, with its records returned as the value they are read for.

    // 1. If IsCallable(predicate) is false, throw a TypeError exception.
    if (!IsCallable(predicate)) ThrowNotAFunction(predicate);

    // 3. Else,
    //    a. Let indices be a List of the integers in the interval from 0 (inclusive) to length (exclusive), in
    //       descending order.
    // 4. For each integer k of indices, do
    for (let k = length - 1; k >= 0; --k) {
        // a. Let propertyKey be ! ToString(𝔽(k)).
        // b. NOTE: If obj is a TypedArray, the following invocation of Get will return a normal completion.
        // c. Let kValue be ? Get(obj, propertyKey).
        const kValue = object[k];

        // d. Let testResult be ? Call(predicate, thisArg, « kValue, 𝔽(k), obj »).
        // e. If ToBoolean(testResult) is true, return the Record { [[Index]]: 𝔽(k), [[Value]]: kValue }.
        if (Call(predicate, thisArg, kValue, k, object)) return k;
    }

    // 5. Return the Record { [[Index]]: -1𝔽, [[Value]]: undefined }.
    return -1;
}

/**
 * 23.1.3.15 Array.prototype.forEach ( callback [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.foreach
 */
function forEach(callback, thisArg) {
    // 1. Let obj be ? ToObject(this value).
    const object = ToObject(this);

    // 2. Let length be ? LengthOfArrayLike(obj).
    const length = ToLength(object.length);

    // 3. If IsCallable(callback) is false, throw a TypeError exception.
    if (!IsCallable(callback)) ThrowNotAFunction(callback);

    // 4. Let k be 0.
    // 5. Repeat, while k < length,
    for (let k = 0; k < length; ++k) {
        // a. Let propertyKey be ! ToString(𝔽(k)).
        // b. Let kPresent be ? HasProperty(obj, propertyKey).
        // c. If kPresent is true, then
        if (k in object) {
            // i. Let kValue be ? Get(obj, propertyKey).
            const kValue = object[k];

            // ii. Perform ? Call(callback, thisArg, « kValue, 𝔽(k), obj »).
            Call(callback, thisArg, kValue, k, object);
        }

        // d. Set k to k + 1.
    }

    // 6. Return undefined.
    return undefined;
}

/**
 * 23.1.3.21 Array.prototype.map ( callback [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.map
 */
function map(callback, thisArg) {
    // 1. Let obj be ? ToObject(this value).
    const object = ToObject(this);

    // 2. Let length be ? LengthOfArrayLike(obj).
    const length = ToLength(object.length);

    // 3. If IsCallable(callback) is false, throw a TypeError exception.
    if (!IsCallable(callback)) ThrowNotAFunction(callback);

    // 4. Let array be ? ArraySpeciesCreate(obj, length).
    const array = ArraySpeciesCreate(object, length);

    // 5. Let k be 0.
    // 6. Repeat, while k < length,
    for (let k = 0; k < length; ++k) {
        // a. Let propertyKey be ! ToString(𝔽(k)).
        // b. Let kPresent be ? HasProperty(obj, propertyKey).
        // c. If kPresent is true, then
        if (k in object) {
            // i. Let kValue be ? Get(obj, propertyKey).
            const kValue = object[k];

            // ii. Let mappedValue be ? Call(callback, thisArg, « kValue, 𝔽(k), obj »).
            const mappedValue = Call(callback, thisArg, kValue, k, object);

            // iii. Perform ? CreateDataPropertyOrThrow(array, propertyKey, mappedValue).
            CreateDataPropertyOrThrow(array, k, mappedValue);
        }

        // d. Set k to k + 1.
    }

    // 7. Return array.
    return array;
}

/**
 * 23.1.3.24 Array.prototype.reduce ( callback [ , initialValue ] ), https://tc39.es/ecma262/#sec-array.prototype.reduce
 */
function reduce(callback, initialValue) {
    // 1. Let obj be ? ToObject(this value).
    const object = ToObject(this);

    // 2. Let length be ? LengthOfArrayLike(obj).
    const length = ToLength(object.length);

    // 3. If IsCallable(callback) is false, throw a TypeError exception.
    if (!IsCallable(callback)) ThrowNotAFunction(callback);

    const initialValueIsPresent = ArgumentCount() > 1;

    // 4. If length = 0 and initialValue is not present, throw a TypeError exception.
    if (length === 0 && !initialValueIsPresent) ThrowTypeError("Reduce of empty array with no initial value");

    // 5. Let k be 0.
    let k = 0;

    // 6. Let accumulator be undefined.
    let accumulator = undefined;

    // 7. If initialValue is present, then
    if (initialValueIsPresent) {
        // a. Set accumulator to initialValue.
        accumulator = initialValue;
    }
    // 8. Else,
    else {
        // a. Let kPresent be false.
        let kPresent = false;

        // b. Repeat, while kPresent is false and k < length,
        while (!kPresent && k < length) {
            // i. Let propertyKey be ! ToString(𝔽(k)).
            // ii. Set kPresent to ? HasProperty(obj, propertyKey).
            kPresent = k in object;

            // iii. If kPresent is true, then
            if (kPresent) {
                // 1. Set accumulator to ? Get(obj, propertyKey).
                accumulator = object[k];
            }

            // iv. Set k to k + 1.
            ++k;
        }

        // c. If kPresent is false, throw a TypeError exception.
        if (!kPresent) ThrowTypeError("Reduce of empty array with no initial value");
    }

    // 9. Repeat, while k < length,
    for (; k < length; ++k) {
        // a. Let propertyKey be ! ToString(𝔽(k)).
        // b. Let kPresent be ? HasProperty(obj, propertyKey).
        // c. If kPresent is true, then
        if (k in object) {
            // i. Let kValue be ? Get(obj, propertyKey).
            const kValue = object[k];

            // ii. Set accumulator to ? Call(callback, undefined, « accumulator, kValue, 𝔽(k), obj »).
            accumulator = Call(callback, undefined, accumulator, kValue, k, object);
        }

        // d. Set k to k + 1.
    }

    // 10. Return accumulator.
    return accumulator;
}

/**
 * 23.1.3.25 Array.prototype.reduceRight ( callback [ , initialValue ] ), https://tc39.es/ecma262/#sec-array.prototype.reduceright
 */
function reduceRight(callback, initialValue) {
    // 1. Let obj be ? ToObject(this value).
    const object = ToObject(this);

    // 2. Let length be ? LengthOfArrayLike(obj).
    const length = ToLength(object.length);

    // 3. If IsCallable(callback) is false, throw a TypeError exception.
    if (!IsCallable(callback)) ThrowNotAFunction(callback);

    const initialValueIsPresent = ArgumentCount() > 1;

    // 4. If length = 0 and initialValue is not present, throw a TypeError exception.
    if (length === 0 && !initialValueIsPresent) ThrowTypeError("Reduce of empty array with no initial value");

    // 5. Let k be length - 1.
    let k = length - 1;

    // 6. Let accumulator be undefined.
    let accumulator = undefined;

    // 7. If initialValue is present, then
    if (initialValueIsPresent) {
        // a. Set accumulator to initialValue.
        accumulator = initialValue;
    }
    // 8. Else,
    else {
        // a. Let kPresent be false.
        let kPresent = false;

        // b. Repeat, while kPresent is false and k ≥ 0,
        while (!kPresent && k >= 0) {
            // i. Let propertyKey be ! ToString(𝔽(k)).
            // ii. Set kPresent to ? HasProperty(obj, propertyKey).
            kPresent = k in object;

            // iii. If kPresent is true, then
            if (kPresent) {
                // 1. Set accumulator to ? Get(obj, propertyKey).
                accumulator = object[k];
            }

            // iv. Set k to k - 1.
            --k;
        }

        // c. If kPresent is false, throw a TypeError exception.
        if (!kPresent) ThrowTypeError("Reduce of empty array with no initial value");
    }

    // 9. Repeat, while k ≥ 0,
    for (; k >= 0; --k) {
        // a. Let propertyKey be ! ToString(𝔽(k)).
        // b. Let kPresent be ? HasProperty(obj, propertyKey).
        // c. If kPresent is true, then
        if (k in object) {
            // i. Let kValue be ? Get(obj, propertyKey).
            const kValue = object[k];

            // ii. Set accumulator to ? Call(callback, undefined, « accumulator, kValue, 𝔽(k), obj »).
            accumulator = Call(callback, undefined, accumulator, kValue, k, object);
        }

        // d. Set k to k - 1.
    }

    // 10. Return accumulator.
    return accumulator;
}

/**
 * 23.1.3.29 Array.prototype.some ( callback [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.some
 */
function some(callback, thisArg) {
    // 1. Let obj be ? ToObject(this value).
    const object = ToObject(this);

    // 2. Let length be ? LengthOfArrayLike(obj).
    const length = ToLength(object.length);

    // 3. If IsCallable(callback) is false, throw a TypeError exception.
    if (!IsCallable(callback)) ThrowNotAFunction(callback);

    // 4. Let k be 0.
    // 5. Repeat, while k < length,
    for (let k = 0; k < length; ++k) {
        // a. Let propertyKey be ! ToString(𝔽(k)).
        // b. Let kPresent be ? HasProperty(obj, propertyKey).
        // c. If kPresent is true, then
        if (k in object) {
            // i. Let kValue be ? Get(obj, propertyKey).
            const kValue = object[k];

            // ii. Let testResult be ToBoolean(? Call(callback, thisArg, « kValue, 𝔽(k), obj »)).
            // iii. If testResult is true, return true.
            if (Call(callback, thisArg, kValue, k, object)) return true;
        }

        // d. Set k to k + 1.
    }

    // 6. Return false.
    return false;
}
