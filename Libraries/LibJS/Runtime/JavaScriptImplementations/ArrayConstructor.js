/**
 * 2.1.1.1 Array.fromAsync ( asyncItems [ , mapper [ , thisArg ] ] ), https://tc39.es/proposal-array-from-async/#sec-array.fromAsync
 */
async function fromAsync(asyncItems, mapper, thisArg) {
    // 1. Let C be the this value.
    const constructor = this;

    let mapping;

    // 2. If mapper is undefined, then
    if (mapper === undefined) {
        // a. Let mapping be false.
        mapping = false;
    }
    // 3. Else,
    else {
        // a. If IsCallable(mapper) is false, throw a TypeError exception.
        if (!IsCallable(mapper)) {
            ThrowTypeError("mapper must be a function");
        }

        // b. Let mapping be true.
        mapping = true;
    }

    // 4. Let usingAsyncIterator be ? GetMethod(asyncItems, %Symbol.asyncIterator%).
    const usingAsyncIterator = GetMethod(asyncItems, SYMBOL_ASYNC_ITERATOR);

    let usingSyncIterator = undefined;

    // 5. If usingAsyncIterator is undefined, then
    if (usingAsyncIterator === undefined) {
        // a. Let usingSyncIterator be ? GetMethod(asyncItems, %Symbol.iterator%).
        usingSyncIterator = GetMethod(asyncItems, SYMBOL_ITERATOR);
    }

    // 6. Let iteratorRecord be undefined.
    let iteratorRecord = undefined;

    // 7. If usingAsyncIterator is not undefined, then
    if (usingAsyncIterator !== undefined) {
        // a. Set iteratorRecord to ? GetIteratorFromMethod(asyncItems, usingAsyncIterator).
        iteratorRecord = GetIteratorFromMethod(asyncItems, usingAsyncIterator);
    }
    // 8. Else if usingSyncIterator is not undefined, then
    else if (usingSyncIterator !== undefined) {
        // a. Set iteratorRecord to CreateAsyncFromSyncIterator(? GetIteratorFromMethod(asyncItems, usingSyncIterator)).
        const iteratorFromMethod = GetIteratorFromMethod(asyncItems, usingSyncIterator);
        iteratorRecord = CreateAsyncFromSyncIterator(
            iteratorFromMethod.iterator,
            iteratorFromMethod.nextMethod,
            iteratorFromMethod.done
        );
    }

    // 9. If iteratorRecord is not undefined, then
    if (iteratorRecord !== undefined) {
        let array;

        // a. If IsConstructor(C) is true, then
        if (IsConstructor(constructor)) {
            // i. Let A be ? Construct(C).
            array = new constructor();
        }
        // b. Else,
        else {
            // i. Let A be ! ArrayCreate(0).
            array = [];
        }

        // c. Let k be 0.
        // d. Repeat,
        for (let k = 0; ; ++k) {
            // i. If k ≥ 2**53 - 1, then
            if (k >= MAX_ARRAY_LIKE_INDEX) {
                // 1. Let error be ThrowCompletion(a newly created TypeError object).
                const error = NewTypeError("Maximum array size exceeded");

                // 2. Return ? AsyncIteratorClose(iteratorRecord, error).
                return AsyncIteratorClose(iteratorRecord, error, true);
            }

            // ii. Let Pk be ! ToString(𝔽(k)).
            // iii. Let nextResult be ? Call(iteratorRecord.[[NextMethod]], iteratorRecord.[[Iterator]]).
            // iv. Set nextResult to ? Await(nextResult).
            const nextResult = await Call(iteratorRecord.nextMethod, iteratorRecord.iterator);

            // v. If nextResult is not an Object, throw a TypeError exception.
            ThrowIfNotObject(nextResult);

            // vi. Let done be ? IteratorComplete(nextResult).
            const done = IteratorComplete(nextResult);

            // vii. If done is true, then
            if (done) {
                // 1. Perform ? Set(A, "length", 𝔽(k), true).
                array.length = k;

                // 2. Return A.
                return array;
            }

            // viii. Let nextValue be ? IteratorValue(nextResult).
            const nextValue = nextResult.value;

            try {
                let mappedValue;

                // ix. If mapping is true, then
                if (mapping) {
                    // 1. Let mappedValue be Completion(Call(mapper, thisArg, « nextValue, 𝔽(k) »)).
                    // 2. IfAbruptCloseAsyncIterator(mappedValue, iteratorRecord).
                    // 3. Set mappedValue to Completion(Await(mappedValue)).
                    // 4. IfAbruptCloseAsyncIterator(mappedValue, iteratorRecord).
                    mappedValue = await Call(mapper, thisArg, nextValue, k);
                }
                // x. Else,
                else {
                    // 1. Let mappedValue be nextValue.
                    mappedValue = nextValue;
                }

                // xi. Let defineStatus be Completion(CreateDataPropertyOrThrow(A, Pk, mappedValue)).
                // xii. IfAbruptCloseAsyncIterator(defineStatus, iteratorRecord).
                CreateDataPropertyOrThrow(array, k, mappedValue);
            } catch (exception) {
                return AsyncIteratorClose(iteratorRecord, exception, true);
            }

            // xiii. Set k to k + 1.
        }
    }
    // 10. Else,
    else {
        // a. NOTE: asyncItems is neither an AsyncIterable nor an Iterable so assume it is an array-like object.
        // b. Let arrayLike be ! ToObject(asyncItems).
        const arrayLike = ToObject(asyncItems);

        // c. Let len be ? LengthOfArrayLike(arrayLike).
        const length = ToLength(arrayLike.length);

        let array;

        // d. If IsConstructor(C) is true, then
        if (IsConstructor(constructor)) {
            // i. Let A be ? Construct(C, « 𝔽(len) »).
            array = new constructor(length);
        }
        // e. Else,
        else {
            // i. Let A be ? ArrayCreate(len).
            array = NewArrayWithLength(length);
        }

        // f. Let k be 0.
        // g. Repeat, while k < len,
        for (let k = 0; k < length; ++k) {
            // i. Let Pk be ! ToString(𝔽(k)).
            // ii. Let kValue be ? Get(arrayLike, Pk).
            // iii. Set kValue to ? Await(kValue).
            const kValue = await arrayLike[k];

            let mappedValue;

            // iv. If mapping is true, then
            if (mapping) {
                // 1. Let mappedValue be ? Call(mapper, thisArg, « kValue, 𝔽(k) »).
                // 2. Set mappedValue to ? Await(mappedValue).
                mappedValue = await Call(mapper, thisArg, kValue, k);
            }
            // v. Else,
            else {
                // 1. Let mappedValue be kValue.
                mappedValue = kValue;
            }

            // vi. Perform ? CreateDataPropertyOrThrow(A, Pk, mappedValue).
            CreateDataPropertyOrThrow(array, k, mappedValue);

            // vii. Set k to k + 1.
        }

        // h. Perform ? Set(A, "length", 𝔽(len), true).
        array.length = length;

        // i. Return A.
        return array;
    }
}
