describe("the prototype property of normal functions that never had it looked at", () => {
    test("it comes right after length and name, whatever is added first", () => {
        function f() {}
        f.g = 1;
        expect(Object.getOwnPropertyNames(f)).toEqual(["length", "name", "prototype", "g"]);

        function h() {}
        h.prototype;
        h.g = 1;
        expect(Object.getOwnPropertyNames(h)).toEqual(["length", "name", "prototype", "g"]);
    });

    test("length and name were looked at first", () => {
        function f(a) {}
        expect(f.length).toBe(1);
        f.g = 1;
        f.h = 2;
        expect(f.prototype.constructor).toBe(f);
        expect(Object.getOwnPropertyNames(f)).toEqual(["length", "name", "prototype", "g", "h"]);
        expect(f.g + f.h).toBe(3);
    });

    test("length and name are looked at last", () => {
        function f(a, b) {}
        f.g = 1;
        f.prototype.x = 1;
        f.h = 2;
        expect(f.name).toBe("f");
        expect(f.length).toBe(2);
        expect(Object.getOwnPropertyNames(f)).toEqual(["length", "name", "prototype", "g", "h"]);
        expect(new f().x).toBe(1);
        expect(f.g + f.h).toBe(3);
    });

    test("it is assigned to first", () => {
        function f() {}
        f.g = 1;
        const prototype = {};
        f.prototype = prototype;
        expect(Object.getOwnPropertyNames(f)).toEqual(["length", "name", "prototype", "g"]);
        expect(Object.getPrototypeOf(new f())).toBe(prototype);
    });

    test("deleting length or name", () => {
        function f() {}
        expect(f.length).toBe(0);
        expect(delete f.name).toBeTrue();
        f.g = 1;
        expect(Object.getOwnPropertyNames(f)).toEqual(["length", "prototype", "g"]);

        function h() {}
        h.g = 1;
        expect(delete h.length).toBeTrue();
        expect(Object.getOwnPropertyNames(h)).toEqual(["name", "prototype", "g"]);

        function k() {}
        expect(delete k.name).toBeTrue();
        Object.defineProperty(k, "name", { value: "other" });
        expect(Object.getOwnPropertyNames(k)).toEqual(["length", "prototype", "name"]);
        expect(k.name).toBe("other");
    });

    test("deleting an added property", () => {
        function f() {}
        f.g = 1;
        f.h = 2;
        expect(delete f.g).toBeTrue();
        expect(Object.getOwnPropertyNames(f)).toEqual(["length", "name", "prototype", "h"]);
        expect(f.h).toBe(2);
    });

    test("preventing extensions", () => {
        function f() {}
        f.g = 1;
        Object.preventExtensions(f);
        expect(Object.getOwnPropertyNames(f)).toEqual(["length", "name", "prototype", "g"]);
        expect(f.prototype.constructor).toBe(f);
        expect(Object.isExtensible(f)).toBeFalse();
    });

    test("changing the prototype of the function", () => {
        function f() {}
        f.g = 1;
        Object.setPrototypeOf(f, null);
        expect(Object.getOwnPropertyNames(f)).toEqual(["length", "name", "prototype", "g"]);
        expect(Object.getPrototypeOf(f)).toBeNull();
    });

    test("many added properties", () => {
        function f() {}
        for (let i = 0; i < 100; ++i) f["p" + i] = i;
        const names = Object.getOwnPropertyNames(f);
        expect(names.length).toBe(103);
        expect(names.slice(0, 4)).toEqual(["length", "name", "prototype", "p0"]);
        for (let i = 0; i < 100; ++i) expect(f["p" + i]).toBe(i);
        expect(f.prototype.constructor).toBe(f);

        function g() {}
        expect(g.name).toBe("g");
        for (let i = 0; i < 100; ++i) g["p" + i] = i;
        expect(g.prototype.constructor).toBe(g);
        expect(Object.getOwnPropertyNames(g).slice(0, 4)).toEqual(["length", "name", "prototype", "p0"]);
        for (let i = 0; i < 100; ++i) expect(g["p" + i]).toBe(i);
    });

    test("a function that is a prototype", () => {
        function f() {}
        f.g = 1;
        const o = Object.create(f);
        expect(o.g).toBe(1);
        expect(Object.getOwnPropertyNames(f)).toEqual(["length", "name", "prototype", "g"]);
        expect(o.prototype).toBe(f.prototype);

        function A() {}
        A.s = 1;
        class B extends A {}
        expect(B.s).toBe(1);
        expect(Object.getOwnPropertyNames(A)).toEqual(["length", "name", "prototype", "s"]);
        expect(new B()).toBeInstanceOf(A);
    });

    test("functions from the Function constructor", () => {
        const f = new Function("a", "b", "");
        f.g = 1;
        expect(Object.getOwnPropertyNames(f)).toEqual(["length", "name", "prototype", "g"]);
        expect(Object.getOwnPropertyDescriptor(f, "prototype")).toEqual({
            value: f.prototype,
            writable: true,
            enumerable: false,
            configurable: false,
        });
        expect(Object.getOwnPropertyDescriptor(f.prototype, "constructor")).toEqual({
            value: f,
            writable: true,
            enumerable: false,
            configurable: true,
        });
        expect(Object.getPrototypeOf(f.prototype)).toBe(Object.prototype);
        expect(new f()).toBeInstanceOf(f);
    });

    test("it shadows enumerable properties of the prototype chain", () => {
        Object.prototype.prototype = 1;
        try {
            function f() {}
            expect(f.length).toBe(0);
            const keys = [];
            for (const key in f) keys.push(key);
            expect(keys).toEqual([]);

            function g() {}
            g.a = 1;
            expect(g.name).toBe("g");
            const otherKeys = [];
            for (const key in g) otherKeys.push(key);
            expect(otherKeys).toEqual(["a"]);
        } finally {
            delete Object.prototype.prototype;
        }
    });

    test("caches of lookups on arrow functions and methods do not apply to it", () => {
        Function.prototype.prototype = 7;
        try {
            const get = f => f.prototype;
            for (let i = 0; i < 10; ++i) {
                expect(get(() => {})).toBe(7);
                expect(get({ m() {} }.m)).toBe(7);
            }
            function f() {}
            expect(get(f).constructor).toBe(f);
        } finally {
            delete Function.prototype.prototype;
        }

        const set = (f, key, value) => {
            f[key] = value;
        };
        for (let i = 0; i < 10; ++i) {
            const arrow = () => {};
            arrow.x = 1;
            set(arrow, "prototype", 1);
        }
        function h() {}
        h.x = 1;
        set(h, "prototype", 1);
        expect(Object.getOwnPropertyNames(h)).toEqual(["length", "name", "prototype", "x"]);
        expect(Object.getOwnPropertyDescriptor(h, "prototype")).toEqual({
            value: 1,
            writable: true,
            enumerable: false,
            configurable: false,
        });
    });

    test("an inherited prototype setter cached from methods is not called for it", () => {
        const receivers = [];
        Object.defineProperty(Function.prototype, "prototype", {
            configurable: true,
            set() {
                receivers.push(this);
            },
        });
        try {
            const set = (f, value) => {
                f.prototype = value;
            };
            const method = { m() {} }.m;
            for (let i = 0; i < 10; ++i) set(method, 1);
            function f() {}
            const prototype = {};
            set(f, prototype);
            expect(receivers.length).toBe(10);
            expect(receivers.includes(f)).toBeFalse();
            expect(Object.getPrototypeOf(new f())).toBe(prototype);
        } finally {
            delete Function.prototype.prototype;
        }
    });

    test("functions made in a loop", () => {
        const make = () => {
            function F(x) {}
            F.a = 1;
            F.prototype.b = 2;
            F.c = 3;
            return F;
        };
        for (let i = 0; i < 3000; ++i) {
            const F = make();
            expect(F.a + F.c + new F().b).toBe(6);
            if (i % 100 === 0) {
                expect(F.length).toBe(1);
                expect(Object.getOwnPropertyNames(F)).toEqual(["length", "name", "prototype", "a", "c"]);
            }
        }
    });
});
