test("Accessor removal during getter execution should bust GetById cache", () => {
    function f(obj, expected) {
        expect(obj.hm).toBe(expected);
    }

    const proto = {
        get hm() {
            delete proto.hm;
            return 123;
        },
    };
    const obj = Object.create(proto);

    f(obj, 123);
    f(obj, undefined);
});

test("Overriding an inherited getter with a data property on an intermediate prototype invalidates prototype-chain cache", () => {
    function f(obj, expected) {
        expect(obj.hm).toBe(expected);
    }

    const proto = {};
    proto.__proto__ = {
        get hm() {
            return 123;
        },
    };
    const obj = Object.create(proto);

    f(obj, 123);
    Object.defineProperty(proto, "hm", { value: 321 });
    f(obj, 321);
});

test("Modifying prototype in dictionary mode should cause prototype-chain validity invalidation (dict-mode prototype is in the middle of prototype chain)", () => {
    function f(obj, expected) {
        expect(obj.hm).toBe(expected);
    }

    const midProto = {};
    midProto.__proto__ = {
        get hm() {
            return 321;
        },
    };

    const proto = {};
    proto.__proto__ = midProto;

    const obj = Object.create(proto);
    // put midProto into dictionary mode
    for (let i = 0; i < 1000; i++) {
        midProto["i" + i] = i;
    }

    f(obj, 321);
    Object.defineProperty(midProto, "hm", {
        get() {
            return 123;
        },
        configurable: true,
    });
    f(obj, 123);
});

test("Modifying prototype in dictionary mode should cause prototype-chain validity invalidation (dict-mode prototype is direct prototype of target object)", () => {
    function f(obj, expected) {
        expect(obj.hm).toBe(expected);
    }

    const proto = {};
    proto.__proto__ = {
        get hm() {
            return 321;
        },
    };

    const obj = Object.create(proto);
    // put proto into dictionary mode
    for (let i = 0; i < 1000; i++) {
        proto["i" + i] = i;
    }

    f(obj, 321);
    Object.defineProperty(proto, "hm", {
        get() {
            return 123;
        },
        configurable: true,
    });
    f(obj, 123);
});

test("Static property lookup cache invalidates missing own properties", () => {
    const object = {
        valueOf() {
            return 1;
        },
    };

    for (let i = 0; i < 10; ++i) expect(+object).toBe(1);

    object[Symbol.toPrimitive] = () => 2;
    expect(+object).toBe(2);
});

test("Static property lookup cache invalidates missing prototype properties", () => {
    const prototype = {
        valueOf() {
            return 1;
        },
    };
    const object = Object.create(prototype);

    for (let i = 0; i < 10; ++i) expect(+object).toBe(1);

    prototype[Symbol.toPrimitive] = () => 2;
    expect(+object).toBe(2);
});

test("GetById cache invalidates missing own properties", () => {
    function read_value(object) {
        return object.value;
    }

    const object = {};
    for (let i = 0; i < 10; ++i) expect(read_value(object)).toBeUndefined();

    object.value = 42;
    expect(read_value(object)).toBe(42);
});

test("GetById cache invalidates missing prototype properties", () => {
    function read_value(object) {
        return object.value;
    }

    const prototype = {};
    const object = Object.create(prototype);
    for (let i = 0; i < 10; ++i) expect(read_value(object)).toBeUndefined();

    prototype.value = 42;
    expect(read_value(object)).toBe(42);
});

test("GetById cache invokes prototype getters with the receiver", () => {
    function read_value(object) {
        return object.value;
    }

    const prototype = {
        get value() {
            return this.payload;
        },
    };
    const object = Object.create(prototype);
    object.payload = 42;

    for (let i = 0; i < 10; ++i) expect(read_value(object)).toBe(42);
});

test("GetById cache propagates getter exceptions to the caller", () => {
    function read_value(object) {
        return object.value;
    }

    let should_throw = false;
    const object = {
        get value() {
            if (should_throw) throw new Error("boom");
            return 42;
        },
    };

    for (let i = 0; i < 10; ++i) expect(read_value(object)).toBe(42);
    should_throw = true;
    expect(() => read_value(object)).toThrowWithMessage(Error, "boom");
    should_throw = false;
    expect(read_value(object)).toBe(42);
});
