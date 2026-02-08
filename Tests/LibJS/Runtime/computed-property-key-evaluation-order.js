test("ToPropertyKey of computed key runs before value expression (Symbol.toPrimitive)", () => {
    var order = [];
    var key = {
        [Symbol.toPrimitive](hint) {
            order.push("key-toPrimitive-" + hint);
            return "computed";
        },
    };
    var obj = {
        [key]: (order.push("value"), 42),
    };
    expect(order).toEqual(["key-toPrimitive-string", "value"]);
    expect(obj.computed).toBe(42);
});

test("ToPropertyKey of computed key runs before value expression (toString)", () => {
    var order = [];
    var key = {
        toString() {
            order.push("key-toString");
            return "computed";
        },
    };
    var obj = {
        [key]: (order.push("value"), 42),
    };
    expect(order).toEqual(["key-toString", "value"]);
    expect(obj.computed).toBe(42);
});

test("ToPropertyKey of computed key runs before value expression (valueOf with null toString)", () => {
    var order = [];
    var key = {
        toString: null,
        valueOf() {
            order.push("key-valueOf");
            return "computed";
        },
    };
    var obj = {
        [key]: (order.push("value"), 42),
    };
    expect(order).toEqual(["key-valueOf", "value"]);
    expect(obj.computed).toBe(42);
});

test("multiple computed keys: each key converts before its own value", () => {
    var order = [];
    var key1 = {
        [Symbol.toPrimitive]() {
            order.push("key1");
            return "a";
        },
    };
    var key2 = {
        [Symbol.toPrimitive]() {
            order.push("key2");
            return "b";
        },
    };
    var obj = {
        [key1]: (order.push("val1"), 1),
        [key2]: (order.push("val2"), 2),
    };
    expect(order).toEqual(["key1", "val1", "key2", "val2"]);
    expect(obj.a).toBe(1);
    expect(obj.b).toBe(2);
});

test("mixed string-literal and computed keys maintain correct order", () => {
    var order = [];
    var key = {
        toString() {
            order.push("key");
            return "b";
        },
    };
    var obj = {
        a: (order.push("val-a"), 1),
        [key]: (order.push("val-b"), 2),
        c: (order.push("val-c"), 3),
    };
    expect(order).toEqual(["val-a", "key", "val-b", "val-c"]);
    expect(obj.a).toBe(1);
    expect(obj.b).toBe(2);
    expect(obj.c).toBe(3);
});

test("computed key throwing prevents value expression from evaluating", () => {
    var valueSideEffect = false;
    var error = new Error("key threw");
    var key = {
        toString() {
            throw error;
        },
    };
    expect(() => {
        var obj = { [key]: (valueSideEffect = true) };
    }).toThrow(error);
    expect(valueSideEffect).toBeFalse();
});

test("Symbol.toPrimitive returning symbol works as computed key", () => {
    var sym = Symbol("test");
    var order = [];
    var key = {
        [Symbol.toPrimitive]() {
            order.push("toPrimitive");
            return sym;
        },
    };
    var obj = {
        [key]: (order.push("value"), 42),
    };
    expect(order).toEqual(["toPrimitive", "value"]);
    expect(obj[sym]).toBe(42);
});

test("computed key with number result", () => {
    var order = [];
    var key = {
        [Symbol.toPrimitive]() {
            order.push("toPrimitive");
            return 42;
        },
    };
    var obj = {
        [key]: (order.push("value"), "hello"),
    };
    expect(order).toEqual(["toPrimitive", "value"]);
    expect(obj[42]).toBe("hello");
});

test("ToPrimitive is called exactly once per computed key", () => {
    var count = 0;
    var key = {
        [Symbol.toPrimitive]() {
            count++;
            return "prop";
        },
    };
    var obj = { [key]: 1 };
    expect(count).toBe(1);
    expect(obj.prop).toBe(1);
});

test("computed getter key evaluates before subsequent properties", () => {
    var order = [];
    var key = {
        toString() {
            order.push("getter-key");
            return "prop";
        },
    };
    var obj = {
        get [key]() {
            return 99;
        },
        after: (order.push("after-val"), 1),
    };
    expect(order).toEqual(["getter-key", "after-val"]);
    expect(obj.prop).toBe(99);
    expect(obj.after).toBe(1);
});

test("computed setter key evaluates before subsequent properties", () => {
    var order = [];
    var key = {
        toString() {
            order.push("setter-key");
            return "prop";
        },
    };
    var captured;
    var obj = {
        set [key](v) {
            captured = v;
        },
        after: (order.push("after-val"), 1),
    };
    expect(order).toEqual(["setter-key", "after-val"]);
    obj.prop = 42;
    expect(captured).toBe(42);
});

test("primitive computed keys are not double-converted", () => {
    // String keys should work directly
    var obj1 = { ["hello"]: 1 };
    expect(obj1.hello).toBe(1);

    // Number keys should work
    var obj2 = { [42]: 2 };
    expect(obj2[42]).toBe(2);

    // Symbol keys should work
    var sym = Symbol("test");
    var obj3 = { [sym]: 3 };
    expect(obj3[sym]).toBe(3);

    // Boolean keys (converted to string)
    var obj4 = { [true]: 4 };
    expect(obj4["true"]).toBe(4);

    // null/undefined keys (converted to string)
    var obj5 = { [null]: 5, [undefined]: 6 };
    expect(obj5["null"]).toBe(5);
    expect(obj5["undefined"]).toBe(6);
});

test("computed key with toString returning toString-getter accessor", () => {
    var order = [];
    var key = {
        get toString() {
            order.push("get-toString");
            return function () {
                order.push("toString-called");
                return "prop";
            };
        },
    };
    var obj = {
        [key]: (order.push("value"), 42),
    };
    expect(order).toEqual(["get-toString", "toString-called", "value"]);
    expect(obj.prop).toBe(42);
});
