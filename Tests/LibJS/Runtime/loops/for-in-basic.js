test("iterate through empty string", () => {
    const a = [];
    for (const property in "") {
        a.push(property);
    }
    expect(a).toEqual([]);
});

test("iterate through number", () => {
    const a = [];
    for (const property in 123) {
        a.push(property);
    }
    expect(a).toEqual([]);
});

test("iterate through empty object", () => {
    const a = [];
    for (const property in {}) {
        a.push(property);
    }
    expect(a).toEqual([]);
});

test("iterate through string", () => {
    const a = [];
    for (const property in "hello") {
        a.push(property);
    }
    expect(a).toEqual(["0", "1", "2", "3", "4"]);
});

test("iterate through typed array", () => {
    const a = [];
    for (const property in new Uint8Array([1, 2])) {
        a.push(property);
    }
    expect(a).toEqual(["0", "1"]);
});

test("array magical length shadows enumerable length higher in prototype chain", () => {
    const tail = { length: 1 };
    const array_prototype = [];
    Object.setPrototypeOf(array_prototype, tail);

    const array = [1, 2];
    Object.setPrototypeOf(array, array_prototype);

    const keys = [];
    for (const key in array) {
        keys.push(key);
    }

    expect(keys).toEqual(["0", "1"]);
});

test("array in prototype chain shadows enumerable length higher up", () => {
    const tail = { length: 1 };
    const array_prototype = [];
    Object.setPrototypeOf(array_prototype, tail);

    const object = Object.create(array_prototype);
    object.foo = 1;

    const keys = [];
    for (const key in object) {
        keys.push(key);
    }

    expect(keys).toEqual(["foo"]);
});

test("array magical length still shadows enumerable length on slow path", () => {
    const tail = { length: 1 };
    const proxy = new Proxy(Object.create(tail), {});

    const array = [1, 2];
    Object.setPrototypeOf(array, proxy);

    const keys = [];
    for (const key in array) {
        keys.push(key);
    }

    expect(keys).toEqual(["0", "1"]);
});

test("same for-in site distinguishes arrays from plain objects with same prototype", () => {
    function collect(object) {
        const keys = [];
        for (const key in object) keys.push(key);
        return keys;
    }

    const proto = Object.create({ length: 1, z: 2 });

    const array = [];
    Object.setPrototypeOf(array, proto);

    const object = {};
    Object.setPrototypeOf(object, proto);

    expect(collect(array)).toEqual(["z"]);
    expect(collect(object)).toEqual(["length", "z"]);
    expect(collect(array)).toEqual(["z"]);
});

test("same for-in site distinguishes plain objects from arrays with same prototype", () => {
    function collect(object) {
        const keys = [];
        for (const key in object) keys.push(key);
        return keys;
    }

    const proto = Object.create({ length: 1, z: 2 });

    const array = [];
    Object.setPrototypeOf(array, proto);

    const object = {};
    Object.setPrototypeOf(object, proto);

    expect(collect(object)).toEqual(["length", "z"]);
    expect(collect(array)).toEqual(["z"]);
    expect(collect(object)).toEqual(["length", "z"]);
});

test("iterate through object", () => {
    const a = [];
    for (const property in { a: 1, b: 2, c: 2 }) {
        a.push(property);
    }
    expect(a).toEqual(["a", "b", "c"]);
});

test("iterate through object with numeric-looking and named properties", () => {
    const object = {};
    object[7] = "seven";
    object.foo = "foo";
    object[2] = "two";
    object.bar = "bar";
    object[9] = "nine";

    const keys = [];
    for (const key in object) {
        keys.push(key);
    }

    expect(keys).toEqual(["2", "7", "9", "foo", "bar"]);
});

test("iterate through undefined", () => {
    for (const property in undefined) {
        expect.fail();
    }
});

test("use already-declared variable", () => {
    var property;
    for (property in "abc");
    expect(property).toBe("2");
});

test("allow binding patterns", () => {
    const expected = [
        ["1", "3", []],
        ["s", undefined, []],
        ["l", "n", ["g", "N", "a", "m", "e"]],
    ];
    let counter = 0;

    for (let [a, , b, ...c] in { 123: 1, sm: 2, longName: 3 }) {
        expect(a).toBe(expected[counter][0]);
        expect(b).toBe(expected[counter][1]);
        expect(c).toEqual(expected[counter][2]);
        counter++;
    }
    expect(counter).toBe(3);
});

describe("special left hand sides", () => {
    test("allow member expression as variable", () => {
        const f = {};
        for (f.a in "abc");
        expect(f.a).toBe("2");
    });

    test("allow member expression of function call", () => {
        const b = {};
        function f() {
            return b;
        }

        for (f().a in "abc");

        expect(f().a).toBe("2");
        expect(b.a).toBe("2");
    });

    test("call expression as for-in LHS is valid in non-strict mode", () => {
        // In non-strict mode, call expressions are allowed as for-in LHS
        // (web compat), but they fail at runtime with ReferenceError.
        expect("for (f() in []);").toEval();
        expect("for (f() in {a: 1}) {}").toEval();
    });

    test("call expression as for-in LHS is SyntaxError in strict mode", () => {
        expect("'use strict'; for (f() in []);").not.toEval();
    });

    test("Cannot change constant declaration in body", () => {
        const vals = [];
        for (const v in [1, 2]) {
            expect(() => v++).toThrowWithMessage(TypeError, "Invalid assignment to const variable");
            vals.push(v);
        }

        expect(vals).toEqual(["0", "1"]);
    });
});

test("remove properties while iterating", () => {
    const from = [1, 2, 3];
    const to = [];
    for (const prop in from) {
        to.push(prop);
        from.pop();
    }
    expect(to).toEqual(["0", "1"]);
});

test("delete future packed index while iterating", () => {
    const from = [1, 2, 3];
    const to = [];

    for (const prop in from) {
        to.push(prop);
        if (prop === "0") delete from[1];
    }

    expect(to).toEqual(["0", "2"]);
});

test("iterate through holey array", () => {
    const from = [1, 2, 3];
    delete from[1];

    const to = [];
    for (const prop in from) {
        to.push(prop);
    }

    expect(to).toEqual(["0", "2"]);
});

test("iterate through sparse array", () => {
    const from = [];
    from[1] = 2;
    from[100] = 3;
    from.foo = 4;

    const to = [];
    for (const prop in from) {
        to.push(prop);
    }

    expect(to).toEqual(["1", "100", "foo"]);
});

test("duplicated properties in prototype", () => {
    const object = { a: 1 };
    const proto = { a: 2 };
    Object.setPrototypeOf(object, proto);
    const a = [];
    for (const prop in object) {
        a.push(prop);
    }
    expect(a).toEqual(["a"]);
});

test("delete future own named property while iterating", () => {
    const object = { a: 1, b: 2, c: 3 };
    const keys = [];

    for (const key in object) {
        keys.push(key);
        if (key === "a") delete object.b;
    }

    expect(keys).toEqual(["a", "c"]);
});

test("delete future prototype named property while iterating", () => {
    const proto = { a: 1, b: 2 };
    const object = Object.create(proto);
    const keys = [];

    for (const key in object) {
        keys.push(key);
        if (key === "a") delete proto.b;
    }

    expect(keys).toEqual(["a"]);
});

test("delete future own dictionary property while iterating", () => {
    const object = {};
    for (let i = 0; i < 70; i++) object["p" + i] = i;

    const keys = [];
    for (const key in object) {
        keys.push(key);
        if (key === "p0") delete object.p1;
        if (key === "p2") delete object.p10;
    }

    expect(keys).not.toContain("p1");
    expect(keys).not.toContain("p10");
    expect(keys).toHaveLength(68);
});

test("delete future prototype dictionary property while iterating", () => {
    const proto = {};
    for (let i = 0; i < 70; i++) proto["p" + i] = i;

    const object = Object.create(proto);
    const keys = [];
    for (const key in object) {
        keys.push(key);
        if (key === "p0") delete proto.p1;
        if (key === "p2") delete proto.p10;
    }

    expect(keys).not.toContain("p1");
    expect(keys).not.toContain("p10");
    expect(keys).toHaveLength(68);
});

test("packed indices stay ahead of named properties", () => {
    const array = [1, 2];
    array.foo = 3;
    array.bar = 4;

    const keys = [];
    for (const key in array) keys.push(key);

    expect(keys).toEqual(["0", "1", "foo", "bar"]);
});

test("delete future own named property while iterating packed indices", () => {
    const array = [1, 2];
    array.foo = 3;
    array.bar = 4;

    const keys = [];
    for (const key in array) {
        keys.push(key);
        if (key === "0") delete array.foo;
    }

    expect(keys).toEqual(["0", "1", "bar"]);
});

test("delete future prototype named property while iterating packed indices", () => {
    const proto = Object.create(Array.prototype);
    proto.foo = 1;
    proto.bar = 2;

    const array = [1, 2];
    Object.setPrototypeOf(array, proto);

    const keys = [];
    for (const key in array) {
        keys.push(key);
        if (key === "0") delete proto.foo;
    }

    expect(keys).toEqual(["0", "1", "bar"]);
});

test("repeated for-in sees own named properties added between runs", () => {
    function collect_twice(object, between_runs) {
        const runs = [];
        for (let pass = 0; pass < 2; ++pass) {
            const keys = [];
            for (const key in object) keys.push(key);
            runs.push(keys);
            if (pass === 0) between_runs();
        }
        return runs;
    }

    const object = { a: 1 };
    const runs = collect_twice(object, () => {
        object.b = 2;
    });

    expect(runs).toEqual([["a"], ["a", "b"]]);
});

test("repeated for-in sees prototype named properties added between runs", () => {
    function collect_twice(object, between_runs) {
        const runs = [];
        for (let pass = 0; pass < 2; ++pass) {
            const keys = [];
            for (const key in object) keys.push(key);
            runs.push(keys);
            if (pass === 0) between_runs();
        }
        return runs;
    }

    const proto = {};
    const object = Object.create(proto);
    const runs = collect_twice(object, () => {
        proto.foo = 1;
    });

    expect(runs).toEqual([[], ["foo"]]);
});

test("repeated for-in sees indexed receiver properties added between runs", () => {
    function collect_twice(object, between_runs) {
        const runs = [];
        for (let pass = 0; pass < 2; ++pass) {
            const keys = [];
            for (const key in object) keys.push(key);
            runs.push(keys);
            if (pass === 0) between_runs();
        }
        return runs;
    }

    const object = { foo: 1 };
    const runs = collect_twice(object, () => {
        object[0] = 2;
    });

    expect(runs).toEqual([["foo"], ["0", "foo"]]);
});

test("repeated for-in sees indexed prototype properties added between runs", () => {
    function collect_twice(object, between_runs) {
        const runs = [];
        for (let pass = 0; pass < 2; ++pass) {
            const keys = [];
            for (const key in object) keys.push(key);
            runs.push(keys);
            if (pass === 0) between_runs();
        }
        return runs;
    }

    const proto = {};
    const object = Object.create(proto);
    const runs = collect_twice(object, () => {
        proto[0] = 1;
    });

    expect(runs).toEqual([[], ["0"]]);
});

test("repeated for-in sees packed receivers grow between runs", () => {
    function collect_twice(object, between_runs) {
        const runs = [];
        for (let pass = 0; pass < 2; ++pass) {
            const keys = [];
            for (const key in object) keys.push(key);
            runs.push(keys);
            if (pass === 0) between_runs();
        }
        return runs;
    }

    const array = [1, 2];
    const runs = collect_twice(array, () => {
        array[2] = 3;
    });

    expect(runs).toEqual([
        ["0", "1"],
        ["0", "1", "2"],
    ]);
});

test("repeated for-in sees packed receivers become holey between runs", () => {
    function collect_twice(object, between_runs) {
        const runs = [];
        for (let pass = 0; pass < 2; ++pass) {
            const keys = [];
            for (const key in object) keys.push(key);
            runs.push(keys);
            if (pass === 0) between_runs();
        }
        return runs;
    }

    const array = [1, 2, 3];
    const runs = collect_twice(array, () => {
        delete array[1];
    });

    expect(runs).toEqual([
        ["0", "1", "2"],
        ["0", "2"],
    ]);
});

test("recursive for-in re-entry keeps the active enumeration stable", () => {
    function collect(object, nested_object) {
        const keys = [];

        for (const key in object) {
            keys.push(key);
            if (nested_object && key === "a") keys.push(...collect(nested_object));
        }

        return keys;
    }

    const outer = { a: 1, b: 2, c: 3 };
    const inner = [1, 2];

    expect(collect(outer, inner)).toEqual(["a", "0", "1", "b", "c"]);
});

test("repeated for-in after break still sees the full next enumeration", () => {
    function collect_twice(object) {
        const runs = [];

        for (let pass = 0; pass < 2; ++pass) {
            const keys = [];
            for (const key in object) {
                keys.push(key);
                if (pass === 0) break;
            }
            runs.push(keys);
        }

        return runs;
    }

    expect(collect_twice({ a: 1, b: 2, c: 3 })).toEqual([["a"], ["a", "b", "c"]]);
});

test("repeated for-in after full named completion still sees the full next enumeration", () => {
    function collect_twice(object) {
        const runs = [];

        for (let pass = 0; pass < 2; ++pass) {
            const keys = [];
            for (const key in object) keys.push(key);
            runs.push(keys);
        }

        return runs;
    }

    expect(collect_twice({ a: 1, b: 2, c: 3 })).toEqual([
        ["a", "b", "c"],
        ["a", "b", "c"],
    ]);
});

test("completed cached for-in does not keep the last receiver alive", () => {
    function exhaust(object) {
        for (const key in object) {
        }
    }

    function exhaust_and_drop_receiver() {
        let receiver = { a: 1, b: 2, c: 3 };
        let weak_ref = new WeakRef(receiver);
        exhaust(receiver);
        return weak_ref;
    }

    let weak_ref = exhaust_and_drop_receiver();
    gc();
    expect(weak_ref.deref()).toBeUndefined();
});

test("repeated for-in after full packed completion still sees the full next enumeration", () => {
    function collect_twice(object) {
        const runs = [];

        for (let pass = 0; pass < 2; ++pass) {
            const keys = [];
            for (const key in object) keys.push(key);
            runs.push(keys);
        }

        return runs;
    }

    const array = [1, 2];
    array.foo = 3;
    array.bar = 4;

    expect(collect_twice(array)).toEqual([
        ["0", "1", "foo", "bar"],
        ["0", "1", "foo", "bar"],
    ]);
});

test("repeated for-in after full mixed numeric-looking and named completion stays stable", () => {
    function collect_twice(object) {
        const runs = [];

        for (let pass = 0; pass < 2; ++pass) {
            const keys = [];
            for (const key in object) keys.push(key);
            runs.push(keys);
        }

        return runs;
    }

    const object = {};
    object[7] = "seven";
    object.foo = "foo";
    object[2] = "two";
    object.bar = "bar";
    object[9] = "nine";

    expect(collect_twice(object)).toEqual([
        ["2", "7", "9", "foo", "bar"],
        ["2", "7", "9", "foo", "bar"],
    ]);
});

test("repeated empty for-in stays empty", () => {
    function collect_twice(object) {
        const runs = [];

        for (let pass = 0; pass < 2; ++pass) {
            const keys = [];
            for (const key in object) keys.push(key);
            runs.push(keys);
        }

        return runs;
    }

    expect(collect_twice({})).toEqual([[], []]);
});

test("shrink packed length while iterating", () => {
    const array = [1, 2, 3];
    const keys = [];

    for (const key in array) {
        keys.push(key);
        if (key === "0") array.length = 1;
    }

    expect(keys).toEqual(["0"]);
});

test("indexed properties on prototype are still enumerated", () => {
    const proto = [1, 2];
    proto.foo = 3;

    const object = Object.create(proto);
    const keys = [];
    for (const key in object) {
        keys.push(key);
    }

    expect(keys).toEqual(["0", "1", "foo"]);
});

test("proxy in prototype chain is still enumerated", () => {
    const proxy = new Proxy({ a: 1, b: 2 }, {});
    const object = Object.create(proxy);
    const keys = [];

    for (const key in object) {
        keys.push(key);
        if (key === "a") delete proxy.b;
    }

    expect(keys).toEqual(["a"]);
});
