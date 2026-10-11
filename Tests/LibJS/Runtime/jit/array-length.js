function compileLength() {
    const lengthOf = new Function("array", "return array.length;");
    jit.prepare(lengthOf);
    for (let i = 0; i < 10; ++i) lengthOf([1, 2, 3]);
    expect(jit.compile(lengthOf)).toBe(jit.enabled);
    return lengthOf;
}

test("compiled array length handles every storage kind and the int32 boundary", () => {
    class Subclass extends Array {}
    const arrays = [[], [1, 2, 3], [1, , 3], Object.freeze([1, 2]), new Subclass(4)];
    const sparse = [];
    sparse[10000] = 1;
    arrays.push(sparse);
    const accessor = [1, 2];
    Object.defineProperty(accessor, 0, {
        get() {
            throw new Error("length must not read elements");
        },
    });
    arrays.push(accessor);
    for (const length of [0, 1, 2147483647, 2147483648, 4294967295]) arrays.push(new Array(length));
    for (const array of arrays) {
        const lengthOf = compileLength();
        expect(lengthOf(array)).toBe(array.length);
    }
});

test("compiled array length handles changed receivers and observable property access", () => {
    let getterCalls = 0;
    let proxyCalls = 0;
    const getter = {
        get length() {
            ++getterCalls;
            return 7;
        },
    };
    const proxy = new Proxy([1, 2], {
        get(target, key) {
            ++proxyCalls;
            expect(key).toBe("length");
            return 19;
        },
    });
    for (const [receiver, expected] of [
        [{ length: "ordinary" }, "ordinary"],
        [getter, 7],
        [proxy, 19],
        ["hello", 5],
        [new String("hello"), 5],
        [new Uint16Array(4), 4],
        [42, undefined],
        [function (a, b) {}, 2],
    ]) {
        const lengthOf = compileLength();
        expect(lengthOf(receiver)).toBe(expected);
        expect(lengthOf([1, 2, 3])).toBe(3);
    }
    expect(getterCalls).toBe(1);
    expect(proxyCalls).toBe(1);
    for (const receiver of [null, undefined]) {
        const lengthOf = compileLength();
        expect(() => lengthOf(receiver)).toThrow(TypeError);
    }
});

test("mixed length feedback keeps ordinary property access", () => {
    const lengthOf = new Function("array", "return array.length;");
    let getterCalls = 0;
    const object = {
        get length() {
            ++getterCalls;
            return 9;
        },
    };
    jit.prepare(lengthOf);
    for (let i = 0; i < 10; ++i) {
        lengthOf([1, 2]);
        lengthOf(object);
        lengthOf("abc");
    }
    expect(jit.compile(lengthOf)).toBe(jit.enabled);
    getterCalls = 0;
    expect(lengthOf(object)).toBe(9);
    expect(getterCalls).toBe(1);
    expect(lengthOf([1, 2])).toBe(2);
    expect(lengthOf("abc")).toBe(3);
});

test("array length loads observe mutations inside loops", () => {
    for (const mutation of ["array.length = 1;", "array.pop();", "alias.length = 1;"]) {
        const scan = new Function(
            "array",
            "alias",
            `let count = 0; for (let i = 0; i < array.length; ++i) { ++count; ${mutation} } return count;`
        );
        jit.prepare(scan);
        for (let i = 0; i < 10; ++i) {
            const array = [1, 2, 3];
            scan(array, array);
        }
        expect(jit.compile(scan)).toBe(jit.enabled);
        const array = [1, 2, 3];
        expect(scan(array, array)).toBe(mutation === "array.pop();" ? 2 : 1);
    }
    const scan = new Function(
        "array",
        "let count = 0; for (let i = 0; i < array.length; ++i) { array[i]; ++count; } return count;"
    );
    jit.prepare(scan);
    for (let i = 0; i < 10; ++i) scan([1, 2, 3]);
    expect(jit.compile(scan)).toBe(jit.enabled);
    const array = [1, 2, 3];
    Object.defineProperty(array, 0, {
        get() {
            array.length = 1;
            return 1;
        },
    });
    expect(scan(array)).toBe(1);
});

test("hoisted scan guards preserve empty scans and changed target properties", () => {
    const scan = new Function(
        "array",
        "target",
        "for (let i = 0; i < array.length; ++i) if (array[i].pos == target.pos) return array[i]; return false;"
    );
    const position = {};
    const first = { pos: {} };
    const last = { pos: position };
    jit.prepare(scan);
    for (let i = 0; i < 10; ++i) scan([first, last], { pos: position });
    expect(jit.compile(scan)).toBe(jit.enabled);
    expect(scan([first, last], { pos: position })).toBe(last);
    expect(scan([], null)).toBeFalse();
    let getterCalls = 0;
    const target = {
        get pos() {
            ++getterCalls;
            return position;
        },
    };
    expect(scan([], target)).toBeFalse();
    expect(getterCalls).toBe(0);
    expect(scan([first, last], target)).toBe(last);
    expect(getterCalls).toBe(2);
});
