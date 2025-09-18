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
