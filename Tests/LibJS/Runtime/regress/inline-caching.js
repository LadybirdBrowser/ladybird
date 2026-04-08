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

test("Repeated missing property access stays undefined after caching", () => {
    function getMissing(obj) {
        return obj.hm;
    }

    // This guards against treating a missing-property cache entry like an own-property hit.
    const obj = { x: 1 };

    expect(getMissing(obj)).toBeUndefined();
    expect(getMissing(obj)).toBeUndefined();
});

test("Adding property on direct prototype invalidates missing-property cache", () => {
    function getMissing(obj) {
        return obj.hm;
    }

    const proto = {};
    const obj = Object.create(proto);

    expect(getMissing(obj)).toBeUndefined();

    proto.hm = 123;

    expect(getMissing(obj)).toBe(123);
    expect(getMissing(obj)).toBe(123);
});

test("Adding property on direct prototype invalidates missing-property cache after GC", () => {
    function getMissing(obj) {
        return obj.hm;
    }

    const proto = {};
    const obj = Object.create(proto);

    expect(getMissing(obj)).toBeUndefined();

    proto.hm = 123;
    gc();

    expect(getMissing(obj)).toBe(123);
    expect(getMissing(obj)).toBe(123);
});

test("Adding property on dictionary-mode prototype invalidates missing-property cache", () => {
    function getMissing(obj) {
        return obj.hm;
    }

    const proto = {};
    for (let i = 0; i < 1000; i++) {
        proto["i" + i] = i;
    }

    const obj = Object.create(proto);

    expect(getMissing(obj)).toBeUndefined();

    proto.hm = 123;

    expect(getMissing(obj)).toBe(123);
    expect(getMissing(obj)).toBe(123);
});

test("Repeated missing .length access stays undefined after caching", () => {
    function getLength(obj) {
        return obj.length;
    }

    // The broken asm fast path would incorrectly read the first named slot here.
    const obj = { x: 1 };

    expect(getLength(obj)).toBeUndefined();
    expect(getLength(obj)).toBeUndefined();
});

test("Missing property cache does not bypass Proxy get trap", () => {
    function getMissing(obj) {
        return obj.hm;
    }

    const proxy = new Proxy(
        {},
        {
            get(target, property, receiver) {
                if (property === "hm") return 123;
                return Reflect.get(target, property, receiver);
            },
        }
    );

    expect(getMissing({})).toBeUndefined();
    expect(getMissing({})).toBeUndefined();
    expect(getMissing(proxy)).toBe(123);
    expect(getMissing(proxy)).toBe(123);
});
