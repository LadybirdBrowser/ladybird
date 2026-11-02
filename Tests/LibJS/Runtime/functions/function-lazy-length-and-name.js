const namesOf = f => Object.getOwnPropertyNames(f).filter(name => name !== "arguments" && name !== "caller");

describe("own properties of functions that never had them looked at", () => {
    test("length and name come first, whatever is added first", () => {
        function a(x, y) {}
        a.foo = 1;
        expect(namesOf(a).slice(0, 2)).toEqual(["length", "name"]);
        expect(a.length).toBe(2);
        expect(a.name).toBe("a");
        expect(namesOf(a)).toContain("foo");

        const b = () => {};
        Object.defineProperty(b, "bar", { value: 1 });
        expect(namesOf(b)).toEqual(["length", "name", "bar"]);

        const c = async x => {};
        c[Symbol.iterator] = 1;
        c.baz = 1;
        expect(namesOf(c)).toEqual(["length", "name", "baz"]);
        expect(c.length).toBe(1);
    });

    test("descriptors", () => {
        const f = function (x) {};
        expect(Object.getOwnPropertyDescriptor(f, "length")).toEqual({
            value: 1,
            writable: false,
            enumerable: false,
            configurable: true,
        });
        const g = function named() {};
        expect(Object.getOwnPropertyDescriptor(g, "name")).toEqual({
            value: "named",
            writable: false,
            enumerable: false,
            configurable: true,
        });
    });

    test("hasOwn and in", () => {
        expect(Object.hasOwn(() => {}, "length")).toBeTrue();
        expect(Object.hasOwn(() => {}, "name")).toBeTrue();
        expect((() => {}).hasOwnProperty("length")).toBeTrue();
        expect("name" in function () {}).toBeTrue();
    });

    test("deleting them reveals the ones of the prototype", () => {
        const f = function named(x) {};
        expect(delete f.name).toBeTrue();
        expect(Object.hasOwn(f, "name")).toBeFalse();
        expect(f.name).toBe("");
        expect(f.length).toBe(1);
        expect(delete f.length).toBeTrue();
        expect(f.length).toBe(0);
    });

    test("they are not writable", () => {
        const f = function named(x) {};
        f.length = 10;
        f.name = "other";
        expect(f.length).toBe(1);
        expect(f.name).toBe("named");

        const g = function named(x) {};
        expect(() => {
            "use strict";
            g.name = "other";
        }).toThrow(TypeError);
        expect(() => Object.assign(function named() {}, { name: "other" })).toThrow(TypeError);
    });

    test("they can be redefined", () => {
        const f = function named() {};
        Object.defineProperty(f, "name", { value: "other" });
        expect(f.name).toBe("other");
        expect(namesOf(f).slice(0, 2)).toEqual(["length", "name"]);
    });

    test("objects inheriting from a function see them", () => {
        const f = function named(x) {};
        const o = Object.create(f);
        expect(o.name).toBe("named");
        expect(o.length).toBe(1);

        // A non-writable inherited property keeps assignments from adding one.
        const g = function other() {};
        const p = Object.create(g);
        p.name = "mine";
        expect(Object.hasOwn(p, "name")).toBeFalse();
        expect(p.name).toBe("other");
    });

    test("they shadow enumerable properties of the prototype chain", () => {
        Object.prototype.name = 1;
        Function.prototype.length2 = 1;
        try {
            const keys = [];
            for (const key in function () {}) keys.push(key);
            expect(keys).not.toContain("name");
            expect(keys).toContain("length2");
        } finally {
            delete Object.prototype.name;
            delete Function.prototype.length2;
        }
    });

    test("freezing, sealing and preventing extensions", () => {
        const frozen = Object.freeze(function named(x) {});
        expect(Object.isFrozen(frozen)).toBeTrue();
        expect(frozen.name).toBe("named");
        expect(Object.getOwnPropertyDescriptor(frozen, "length").configurable).toBeFalse();

        const sealed = Object.seal(() => {});
        expect(Object.isSealed(sealed)).toBeTrue();
        expect(sealed.length).toBe(0);

        const fixed = Object.preventExtensions(function named(x) {});
        expect(fixed.length).toBe(1);
        expect(fixed.name).toBe("named");
        expect(Object.isExtensible(fixed)).toBeFalse();
    });

    test("changing the prototype first", () => {
        const f = function named(x) {};
        Object.setPrototypeOf(f, null);
        expect(f.length).toBe(1);
        expect(f.name).toBe("named");
        expect(Object.getOwnPropertyNames(f).slice(0, 2)).toEqual(["length", "name"]);
    });

    test("a function that is a prototype, before and after", () => {
        const f = function named(x) {};
        const o = Object.create(f);
        f.foo = 1;
        expect(o.foo).toBe(1);
        expect(o.length).toBe(1);
        expect(namesOf(f).slice(0, 2)).toEqual(["length", "name"]);
    });

    test("bind", () => {
        const f = function named(x, y) {};
        const bound = f.bind(null, 1);
        expect(bound.length).toBe(1);
        expect(bound.name).toBe("bound named");
    });

    test("classes", () => {
        class A {
            static x = 1;
            static m() {}
        }
        expect(A.name).toBe("A");
        expect(A.length).toBe(0);
        expect(namesOf(A).slice(0, 3)).toEqual(["length", "name", "prototype"]);
        expect(A.m.name).toBe("m");
        expect(namesOf(A.m)).toEqual(["length", "name"]);

        class B extends A {
            constructor(a, b) {
                super();
            }
        }
        expect(B.length).toBe(2);
        expect(B.name).toBe("B");
    });

    test("names set by the context", () => {
        const a = () => {};
        const o = { b: () => {}, ["c" + 1]: function () {}, get d() {} };
        expect(a.name).toBe("a");
        expect(o.b.name).toBe("b");
        expect(o.c1.name).toBe("c1");
        expect(Object.getOwnPropertyDescriptor(o, "d").get.name).toBe("get d");
        const k = Symbol("s");
        const p = { [k]: () => {} };
        expect(p[k].name).toBe("[s]");
    });

    test("properties added before the first look at them move behind them", () => {
        const f = function named(x) {};
        f.a = 1;
        f.b = 2;
        Object.defineProperty(f, "c", { value: 3, enumerable: false });
        expect(f.a).toBe(1);
        expect(f.name).toBe("named");
        expect(f.a).toBe(1);
        expect(f.b).toBe(2);
        expect(f.c).toBe(3);
        expect(f.length).toBe(1);
        expect(namesOf(f).filter(name => name !== "prototype")).toEqual(["length", "name", "a", "b", "c"]);
        expect(Object.getOwnPropertyDescriptor(f, "c").enumerable).toBeFalse();
        expect(Object.keys(f)).toEqual(["a", "b"]);
    });

    test("deleting an added property", () => {
        const f = () => {};
        f.a = 1;
        f.b = 2;
        expect(delete f.a).toBeTrue();
        expect(namesOf(f)).toEqual(["length", "name", "b"]);
        expect(f.b).toBe(2);
    });

    test("many added properties", () => {
        const f = () => {};
        for (let i = 0; i < 100; ++i) f["p" + i] = i;
        const names = namesOf(f);
        expect(names.length).toBe(102);
        expect(names.slice(0, 3)).toEqual(["length", "name", "p0"]);
        for (let i = 0; i < 100; ++i) expect(f["p" + i]).toBe(i);
    });

    test("accessors and changed attributes of added properties survive", () => {
        const f = () => {};
        let got = 0;
        Object.defineProperty(f, "a", {
            get() {
                return ++got;
            },
            configurable: true,
        });
        f.b = 1;
        Object.defineProperty(f, "b", { writable: false });
        expect(f.name).toBe("f");
        expect(f.a).toBe(1);
        expect(Object.getOwnPropertyDescriptor(f, "b").writable).toBeFalse();
    });

    test("objects inheriting from a function with added properties", () => {
        const f = function named(x) {};
        f.a = 1;
        const o = Object.create(f);
        expect(o.a).toBe(1);
        expect(o.name).toBe("named");
        expect(namesOf(f).slice(0, 3)).toEqual(["length", "name", "a"]);
    });

    test("the prototype property does not get ahead of them", () => {
        function F(a, b) {}
        F.prototype.x = 1;
        expect(new F().x).toBe(1);
        expect(F.length).toBe(2);
        expect(namesOf(F)).toEqual(["length", "name", "prototype"]);
    });

    test("cached additions keep functions apart", () => {
        const addTo = f => {
            f.a = 1;
            f.b = 2;
            return f;
        };
        for (let i = 0; i < 3000; ++i) {
            const f = addTo(x => x);
            if (i % 3 === 0) expect(f.length).toBe(1);
            if (i % 3 === 1) expect(namesOf(f)).toEqual(["length", "name", "a", "b"]);
            expect(f.a + f.b).toBe(3);
            if (i % 7 === 0) {
                expect(f.name).toBe("");
                expect(f.a + f.b).toBe(3);
            }
        }
    });

    test("inline caches do not mix functions up", () => {
        const lengthOf = f => f.length;
        const nameOf = f => f.name;
        const addTo = f => {
            f.foo = 1;
            return f;
        };
        for (let i = 0; i < 2000; ++i) {
            const f = function named(x, y) {};
            expect(lengthOf(f)).toBe(2);
            expect(nameOf(f)).toBe("named");
            const g = addTo(() => {});
            expect(namesOf(g)).toEqual(["length", "name", "foo"]);
            expect(g.foo).toBe(1);
            expect(lengthOf(addTo(x => x))).toBe(1);
        }
    });

    test("a closure copied in a loop", () => {
        const make = () => x => x;
        let last;
        for (let i = 0; i < 20000; ++i) {
            last = make();
            if (i % 1000 === 0) {
                expect(last.length).toBe(1);
                expect(namesOf(last)).toEqual(["length", "name"]);
            }
        }
        expect(last.length).toBe(1);
        expect(last.name).toBe("");
    });
});
