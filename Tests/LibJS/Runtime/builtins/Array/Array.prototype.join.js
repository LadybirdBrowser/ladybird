test("length is 1", () => {
    expect(Array.prototype.join).toHaveLength(1);
});

test("basic functionality", () => {
    expect(["hello", "friends"].join()).toBe("hello,friends");
    expect(["hello", "friends"].join(undefined)).toBe("hello,friends");
    expect(["hello", "friends"].join(" ")).toBe("hello friends");
    expect(["hello", "friends", "foo"].join("~", "#")).toBe("hello~friends~foo");
    expect([].join()).toBe("");
    expect([null].join()).toBe("");
    expect([undefined].join()).toBe("");
    expect([undefined, null, ""].join()).toBe(",,");
    expect([1, null, 2, undefined, 3].join()).toBe("1,,2,,3");
    expect(Array(3).join()).toBe(",,");
});

test("circular references", () => {
    const a = ["foo", [], [1, 2, []], ["bar"]];
    a[1] = a;
    a[2][2] = a;
    // [ "foo", <circular>, [ 1, 2, <circular> ], [ "bar" ] ]
    expect(a.join()).toBe("foo,,1,2,,bar");
});

describe("elements read while joining", () => {
    test("elements that change the array while it is joined", () => {
        const array = [1, 2, 3];
        const changer = {
            toString() {
                array.length = 1;
                return "changed";
            },
        };
        array[1] = changer;
        expect(array.join()).toBe("1,changed,");

        const growing = ["a"];
        growing.push({
            toString() {
                growing.push("late");
                return "b";
            },
        });
        expect(growing.join("-")).toBe("a-b");
    });

    test("holes, nullish values, numbers and strings", () => {
        const holey = [1, , 3];
        expect(holey.join()).toBe("1,,3");
        Array.prototype[1] = "inherited";
        try {
            expect(holey.join()).toBe("1,inherited,3");
        } finally {
            delete Array.prototype[1];
        }
        expect([null, undefined, 0, -1, 2147483647, 1.5, "s", true].join(" ")).toBe("  0 -1 2147483647 1.5 s true");
        expect(["a", "b"].join(["x", "y"])).toBe("ax,yb");
    });

    test("array-likes and arrays with getters", () => {
        expect(Array.prototype.join.call({ length: 2, 0: "a", 1: "b" })).toBe("a,b");
        const array = [1, 2];
        Object.defineProperty(array, 0, { get: () => "getter" });
        expect(array.join()).toBe("getter,2");
    });
});

test("objects whose conversion is not a string", () => {
    const object = {
        toString() {
            return 42;
        },
    };
    let calls = 0;
    const counted = {
        toString() {
            ++calls;
            return {};
        },
        valueOf() {
            return 7;
        },
    };
    expect([object, counted].join()).toBe("42,7");
    expect(calls).toBe(1);
    expect(() => [Symbol("s")].join()).toThrow(TypeError);
    expect([1n, true].join()).toBe("1,true");
});

test("an unused separator is still converted for empty and single-element joins", () => {
    let calls = 0;
    const separator = {
        toString() {
            ++calls;
            return "separator".repeat(8192);
        },
    };
    expect([].join(separator)).toBe("");
    expect(["element"].join(separator)).toBe("element");
    expect(calls).toBe(2);
});
