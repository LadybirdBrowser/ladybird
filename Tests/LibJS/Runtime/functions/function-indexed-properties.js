describe("indexed properties of functions", () => {
    test("functions keep working with indexed properties", () => {
        const outer = 42;
        function f(a) {
            return a + outer;
        }

        f[0] = "zero";
        f[1] = "one";
        f[1000000] = "sparse";

        expect(f[0]).toBe("zero");
        expect(f[1]).toBe("one");
        expect(f[1000000]).toBe("sparse");
        expect(f[2]).toBeUndefined();
        expect(f(1)).toBe(43);
        expect(new f(1)).toBeInstanceOf(f);
    });

    test("indexed keys come first, in order", () => {
        function f() {}
        f.named = 1;
        f[2] = "two";
        f[0] = "zero";

        expect(Object.keys(f)).toEqual(["0", "2", "named"]);
        expect(Object.getOwnPropertyNames(f).slice(0, 2)).toEqual(["0", "2"]);
    });

    test("dense fills stay readable", () => {
        function f() {
            return "called";
        }
        for (let i = 0; i < 1000; ++i) f[i] = i * 2;
        let sum = 0;
        for (let i = 0; i < 1000; ++i) sum += f[i];

        expect(sum).toBe(999000);
        expect(f()).toBe("called");
    });

    test("deleting and redefining", () => {
        function f() {}
        f[0] = 1;
        expect(delete f[0]).toBeTrue();
        expect(f[0]).toBeUndefined();
        expect(0 in f).toBeFalse();

        Object.defineProperty(f, 3, { value: "fixed", writable: false, enumerable: false, configurable: false });
        expect(Object.getOwnPropertyDescriptor(f, 3).writable).toBeFalse();
        expect(delete f[3]).toBeFalse();
        expect(f[3]).toBe("fixed");
    });

    test("freezing", () => {
        function f() {}
        f[0] = 1;
        Object.freeze(f);
        expect(Object.isFrozen(f)).toBeTrue();
        expect(() => {
            "use strict";
            f[0] = 2;
        }).toThrow(TypeError);
        expect(f[0]).toBe(1);
    });

    test("array methods applied to functions", () => {
        function f() {}
        f[0] = "a";
        f[1] = "b";
        // NB: Function length is not writable, so the methods that write it throw.
        expect(Array.prototype.join.call(f)).toBe("");
        expect(() => Array.prototype.push.call(f, "c")).toThrow(TypeError);
        expect(f[0]).toBe("c");
        expect(f[1]).toBe("b");
    });

    test("every kind of function, surviving a collection", () => {
        const functions = [
            function () {},
            () => {},
            function* () {},
            async function () {},
            class {},
            class {
                static #x = 1;
                static m() {
                    return this.#x;
                }
            },
        ];
        for (const f of functions) {
            f[0] = { value: "kept" };
            f[100000] = { value: "sparse" };
        }
        gc();
        for (const f of functions) {
            expect(f[0].value).toBe("kept");
            expect(f[100000].value).toBe("sparse");
        }
        expect(functions[5].m()).toBe(1);
    });

    test("the prototype chain reads the elements of a function", () => {
        function f() {}
        f[7] = "inherited";
        const object = Object.create(f);
        expect(object[7]).toBe("inherited");
        expect(7 in object).toBeTrue();
    });

    test("many functions with elements, collected", () => {
        for (let i = 0; i < 1000; ++i) {
            const f = function () {};
            f[0] = i;
        }
        gc();
    });
});
