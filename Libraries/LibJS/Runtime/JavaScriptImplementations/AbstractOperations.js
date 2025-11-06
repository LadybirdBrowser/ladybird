/**
 * 7.3.10 GetMethod ( V, P ), https://tc39.es/ecma262/#sec-getmethod
 */
function GetMethod(value, property) {
    // 1. Let func be ? GetV(V, P).
    const function_ = value[property];

    // 2. If func is either undefined or null, return undefined.
    if (function_ === undefined || function_ === null) return undefined;

    // 3. If IsCallable(func) is false, throw a TypeError exception.
    if (!IsCallable(function_)) ThrowTypeError("Not a function");

    // 4. Return func.
    return function_;
}

/**
 * 7.4.2 GetIteratorDirect ( obj ), https://tc39.es/ecma262/#sec-getiteratordirect
 */
function GetIteratorDirect(object) {
    // 1. Let nextMethod be ? Get(obj, "next").
    const nextMethod = object.next;

    // 2. Let iteratorRecord be the Iterator Record { [[Iterator]]: obj, [[NextMethod]]: nextMethod, [[Done]]: false }.
    const iteratorRecord = NewObjectWithNoPrototype();
    iteratorRecord.iterator = object;
    iteratorRecord.nextMethod = nextMethod;
    iteratorRecord.done = false;

    // 3. Return iteratorRecord.
    return iteratorRecord;
}

/**
 * 7.4.3 GetIteratorFromMethod ( obj, method ), https://tc39.es/ecma262/#sec-getiteratorfrommethod
 */
function GetIteratorFromMethod(object, method) {
    // 1. Let iterator be ? Call(method, obj).
    const iterator = Call(method, object);

    // 2. If iterator is not an Object, throw a TypeError exception.
    ThrowIfNotObject(iterator);

    // 3. Return ? GetIteratorDirect(iterator).
    return GetIteratorDirect(iterator);
}

/**
 * 7.4.7 IteratorComplete ( iteratorResult ) - https://tc39.es/ecma262/#sec-iteratorcomplete
 */
function IteratorComplete(iteratorResult) {
    // 1. Return ToBoolean(? Get(iteratorResult, "done")).
    return ToBoolean(iteratorResult.done);
}

/**
 * 7.4.13 AsyncIteratorClose ( iteratorRecord, completion ) - https://tc39.es/ecma262/#sec-asynciteratorclose
 */
async function AsyncIteratorClose(iteratorRecord, completionValue, isThrowCompletion) {
    // FIXME: 1. Assert: iteratorRecord.[[Iterator]] is an Object.

    // 2. Let iterator be iteratorRecord.[[Iterator]].
    const iterator = iteratorRecord.iterator;

    let innerResult;

    try {
        // 3. Let innerResult be Completion(GetMethod(iterator, "return")).
        innerResult = GetMethod(iterator, "return");

        // 4. If innerResult is a normal completion, then
        // a. Let return be innerResult.[[Value]].
        // b. If return is undefined, return ? completion.
        // NOTE: If isThrowCompletion is true, it will override this return in the finally block.
        if (innerResult === undefined) return completionValue;

        // c. Set innerResult to Completion(Call(return, iterator)).
        // d. If innerResult is a normal completion, set innerResult to Completion(Await(innerResult.[[Value]])).
        innerResult = await Call(innerResult, iterator);
    } finally {
        // 5. If completion is a throw completion, return ? completion.
        if (isThrowCompletion) throw completionValue;

        // 6. If innerResult is a throw completion, return ? innerResult.
        // NOTE: If the try block threw, it will rethrow when leaving this finally block.
    }

    // 7. If innerResult.[[Value]] is not an Object, throw a TypeError exception.
    ThrowIfNotObject(innerResult);

    // 8. Return ? completion.
    // NOTE: Because of step 5, this will not be a throw completion.
    return completionValue;
}
