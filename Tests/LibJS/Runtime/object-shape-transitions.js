function keys(object) {
    return Object.getOwnPropertyNames(object);
}

test("objects built the same way see their own properties", () => {
    function make(value) {
        const object = {};
        object.a = value;
        object.b = value + 1;
        return object;
    }
    const first = make(1);
    const second = make(10);

    expect(keys(first)).toEqual(["a", "b"]);
    expect(second.a).toBe(10);
    expect(second.b).toBe(11);
});

test("a transition with other attributes is not the same transition", () => {
    const writable = {};
    writable.x = 1;
    const fixed = {};
    Object.defineProperty(fixed, "x", { value: 2, writable: false, enumerable: true, configurable: true });

    expect(Object.getOwnPropertyDescriptor(writable, "x").writable).toBeTrue();
    expect(Object.getOwnPropertyDescriptor(fixed, "x").writable).toBeFalse();

    const writableAgain = {};
    writableAgain.x = 3;
    expect(Object.getOwnPropertyDescriptor(writableAgain, "x").writable).toBeTrue();
    expect(writableAgain.x).toBe(3);
});

test("branching from one shape keeps every branch apart", () => {
    const branches = ["p", "q", "r", "s", "p", "q", "r", "s"].map(key => {
        const object = { shared: 1 };
        object[key] = key;
        return object;
    });
    branches.forEach((object, index) => {
        expect(keys(object)).toEqual(["shared", ["p", "q", "r", "s"][index % 4]]);
    });
});

test("changing attributes after adding a property", () => {
    const makeConfigured = () => {
        const object = { a: 1, b: 2 };
        Object.defineProperty(object, "a", { enumerable: false });
        return object;
    };
    expect(Object.keys(makeConfigured())).toEqual(["b"]);
    expect(Object.keys(makeConfigured())).toEqual(["b"]);

    const plain = { a: 1, b: 2 };
    expect(Object.keys(plain)).toEqual(["a", "b"]);
    plain.c = 3;
    expect(Object.keys(plain)).toEqual(["a", "b", "c"]);
});

test("a transition whose target died is made again", () => {
    const parent = { base: 1 };
    const roots = [];
    for (let round = 0; round < 3; ++round) {
        for (let i = 0; i < 50; ++i) {
            const object = { base: 1 };
            object["transient" + round] = i;
        }
        gc();
        const object = { base: 1 };
        object["transient" + round] = round;
        roots.push(object);
    }
    roots.forEach((object, round) => {
        expect(keys(object)).toEqual(["base", "transient" + round]);
        expect(object["transient" + round]).toBe(round);
    });
    expect(parent.base).toBe(1);
});

test("caches on a shape with a single transition keep it", () => {
    const makeNext = () => {
        const object = { a: 1 };
        object.b = 2;
        return object;
    };
    const before = makeNext();

    const source = { a: 1 };
    expect(Object.keys(source)).toEqual(["a"]);
    expect(Object.assign({}, source)).toEqual({ a: 1 });
    for (const key in source) expect(key).toBe("a");
    expect(String(source)).toBe("[object Object]");
    expect(JSON.stringify(source)).toBe('{"a":1}');

    const after = makeNext();
    expect(keys(before)).toEqual(["a", "b"]);
    expect(keys(after)).toEqual(["a", "b"]);
    expect(after.b).toBe(2);

    const other = { a: 1 };
    other.c = 3;
    expect(keys(other)).toEqual(["a", "c"]);
    expect(makeNext().b).toBe(2);
});

test("dictionaries, deleted properties and many properties", () => {
    const object = {};
    for (let i = 0; i < 200; ++i) object["key" + i] = i;
    delete object.key5;
    object.key5 = "again";
    delete object.key6;

    expect(object.key5).toBe("again");
    expect(object.key6).toBeUndefined();
    expect("key6" in object).toBeFalse();
    expect(keys(object).length).toBe(199);
    expect(keys(object).at(-1)).toBe("key5");
    gc();
    expect(object.key199).toBe(199);

    const prototype = {};
    const child = Object.create(prototype);
    for (let i = 0; i < 50; ++i) prototype["p" + i] = i;
    delete prototype.p3;
    expect(child.p7).toBe(7);
    expect(child.p3).toBeUndefined();
    Object.setPrototypeOf(child, null);
    expect(child.p7).toBeUndefined();
});
