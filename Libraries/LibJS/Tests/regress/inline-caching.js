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
