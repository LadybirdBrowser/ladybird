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
