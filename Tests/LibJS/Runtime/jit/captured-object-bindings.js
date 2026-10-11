function compileCaptured(f, warmUp = f => f()) {
    jit.prepare(f);
    for (let i = 0; i < 10; ++i) warmUp(f);
    expect(jit.compile(f)).toBe(jit.enabled);
    return f;
}

function capturedArray(value) {
    var array = new Int32Array([value]);
    return {
        read: () => array[0],
        replace: value => {
            array = value;
        },
    };
}

test("captured typed arrays keep observing writes to their elements", () => {
    const closure = capturedArray(123);
    const read = compileCaptured(closure.read);
    expect(read()).toBe(123);
    closure.replace(new Int32Array([456]));
    expect(read()).toBe(456);
    closure.replace([789]);
    expect(read()).toBe(789);
    closure.replace(null);
    expect(read).toThrow(TypeError);
});

test("closures sharing compiled code read their own captured objects", () => {
    const first = capturedArray(12);
    const second = capturedArray(34);
    compileCaptured(first.read);
    expect(first.read()).toBe(12);
    expect(second.read()).toBe(34);
    first.replace(new Int32Array([56]));
    expect(second.read()).toBe(34);
    expect(first.read()).toBe(56);
});

test("captured objects are reloaded after calls that replace them", () => {
    var object = { value: 1 };
    function read(callback) {
        const before = object.value;
        callback();
        return [before, object.value];
    }
    compileCaptured(read, f => f(() => {}));
    expect(read(() => (object = { value: 2 }))).toEqual([1, 2]);
    expect(read(() => (object = { value: 3 }))).toEqual([2, 3]);
});

test("captured objects are reloaded after stores in a compiled loop", () => {
    var object = { value: 10 };
    function sum(count, replace) {
        let result = 0;
        for (let i = 0; i < count; ++i) {
            result += object.value;
            if (i === 2 && replace) object = { value: 20 };
        }
        return result;
    }
    compileCaptured(sum, f => f(8, false));
    expect(sum(8, true)).toBe(130);
    expect(sum(8, false)).toBe(160);
});

test("captured functions and outer environments retain their identity", () => {
    function outer(value) {
        var callable = () => value;
        return function middle() {
            return {
                call: () => callable(),
                replace: replacement => {
                    callable = replacement;
                },
            };
        };
    }
    const closure = outer(17)();
    const call = compileCaptured(closure.call);
    expect(call()).toBe(17);
    closure.replace(() => 29);
    expect(call()).toBe(29);
});

test("captured constants survive garbage collection and buffer detachment", () => {
    function factory() {
        var array = new Int32Array([42]);
        return { array, read: () => array[0] };
    }
    const closure = factory();
    compileCaptured(closure.read);
    gc();
    expect(closure.read()).toBe(42);
    detachArrayBuffer(closure.array.buffer);
    expect(closure.read()).toBeUndefined();
});

test("local environments and uninitialized captured bindings preserve their behavior", () => {
    var object = { value: 1 };
    function local(value) {
        {
            let object = { value };
            return (() => object.value)();
        }
    }
    compileCaptured(local, f => f(2));
    expect(local(3)).toBe(3);
    function uninitialized() {
        const read = () => object;
        jit.prepare(read);
        jit.compile(read);
        expect(read).toThrow(ReferenceError);
        let object = { value: 4 };
        return read();
    }
    expect(uninitialized()).toEqual({ value: 4 });
    expect(object.value).toBe(1);
});

// NB: The factories come from new Function, so that each run of a test (see LIBJS_TEST_REPEAT) has new functions
//     whose closures it counts from zero.
function readerFactory() {
    return new Function(
        "value",
        "var array = new Int32Array([value]); return { read: () => array[0], replace: value => { array = value; } };"
    );
}

function exitKinds(f) {
    return jit.exitSites(f).map(site => site.split("@")[0]);
}

test("the only closure of a function speculates on its captured objects once", () => {
    const closure = readerFactory()(5);
    compileCaptured(closure.read);
    expect(closure.read()).toBe(5);
    closure.replace(new Int32Array([6]));
    for (let i = 0; i < 20; ++i) expect(closure.read()).toBe(6);
    if (!jit.enabled || jit.forcesExits) return;
    expect(exitKinds(closure.read)).toContain("UnexpectedValue");
    expect(jit.discardCount(closure.read)).toBeLessThanOrEqual(1);
    expect(jit.compile(closure.read)).toBeTrue();
    for (let i = 0; i < 20; ++i) expect(closure.read()).toBe(6);
    expect(jit.exitCount(closure.read)).toBe(0);
});

test("a captured object that changed is not speculated on at any of its loads", () => {
    const closure = new Function(
        "var object = { value: 1 }; return { read: first => (first ? object.value : object.value * 2), replace: value => { object = value; } };"
    )();
    compileCaptured(closure.read, f => {
        f(true);
        f(false);
    });
    closure.replace({ value: 3 });
    expect(closure.read(true)).toBe(3);
    if (!jit.enabled || jit.forcesExits) return;
    expect(exitKinds(closure.read)).toContain("UnexpectedValue");
    jit.discard(closure.read);
    expect(jit.compile(closure.read)).toBeTrue();
    expect(closure.read(false)).toBe(6);
    expect(jit.exitCount(closure.read)).toBe(0);
});

test("closures of one function do not speculate on each other's captured objects", () => {
    const makeReader = readerFactory();
    const readers = [];
    for (let i = 0; i < 32; ++i) readers.push(makeReader(i).read);
    compileCaptured(readers[0], () => readers.forEach(read => read()));
    let sum = 0;
    for (let round = 0; round < 20; ++round) for (const read of readers) sum += read();
    expect(sum).toBe(20 * ((31 * 32) / 2));
    if (!jit.enabled || jit.forcesExits) return;
    expect(jit.exitCount(readers[0])).toBe(0);
    expect(jit.discardCount(readers[0])).toBe(0);
    expect(exitKinds(readers[0])).not.toContain("UnexpectedValue");
});

test("closures created after a compile stop the speculation on captured objects", () => {
    const makeReader = readerFactory();
    const first = makeReader(0).read;
    compileCaptured(first);
    const readers = [first];
    for (let i = 1; i < 32; ++i) readers.push(makeReader(i).read);
    let sum = 0;
    for (let round = 0; round < 20; ++round) for (const read of readers) sum += read();
    expect(sum).toBe(20 * ((31 * 32) / 2));
    if (!jit.enabled || jit.forcesExits) return;
    expect(jit.discardCount(first)).toBeLessThanOrEqual(1);
    expect(jit.compile(first)).toBeTrue();
    for (let round = 0; round < 20; ++round) for (const read of readers) sum += read();
    expect(jit.exitCount(first)).toBe(0);
});
