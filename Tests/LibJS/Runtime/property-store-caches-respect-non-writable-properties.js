describe("cached property stores do not write to non-writable properties", () => {
    function defineSetter(object) {
        Object.defineProperty(object, "x", { set(value) {} });
        return object;
    }

    function defineNonWritable(object) {
        Object.defineProperty(object, "x", { value: 1 });
        return object;
    }

    test("named key in strict mode", () => {
        function put(object, value) {
            "use strict";
            object.x = value;
        }
        put(defineSetter({}), 7);
        const victim = defineNonWritable({});
        expect(() => put(victim, 8)).toThrow(TypeError);
        expect(victim.x).toBe(1);
    });

    test("named key in sloppy mode", () => {
        function put(object, value) {
            object.x = value;
        }
        put(defineSetter({}), 7);
        const victim = defineNonWritable({});
        put(victim, 8);
        expect(victim.x).toBe(1);
    });

    test("setter that redefines itself as a non-writable property", () => {
        function put(object, value) {
            "use strict";
            object.x = value;
        }
        const object = {};
        Object.defineProperty(object, "x", {
            configurable: true,
            set(value) {
                Object.defineProperty(this, "x", { value: 1 });
            },
        });
        put(object, 5);
        expect(object.x).toBe(1);
        expect(() => put(object, 9)).toThrow(TypeError);
        expect(object.x).toBe(1);
    });

    test("global variable", () => {
        Object.defineProperty(globalThis, "nonWritableGlobalAfterSetter", {
            configurable: true,
            set(value) {
                Object.defineProperty(globalThis, "nonWritableGlobalAfterSetter", { value: 1, configurable: true });
            },
        });
        function put(value) {
            "use strict";
            nonWritableGlobalAfterSetter = value;
        }
        put(5);
        expect(nonWritableGlobalAfterSetter).toBe(1);
        expect(() => put(9)).toThrow(TypeError);
        expect(nonWritableGlobalAfterSetter).toBe(1);
    });

    test("global variable update in strict mode", () => {
        Object.defineProperty(globalThis, "nonWritableGlobalAfterSetterUpdate", {
            configurable: true,
            set(value) {
                Object.defineProperty(globalThis, "nonWritableGlobalAfterSetterUpdate", {
                    value: 1,
                    configurable: true,
                });
            },
        });
        function increment() {
            "use strict";
            nonWritableGlobalAfterSetterUpdate++;
        }
        increment();
        expect(nonWritableGlobalAfterSetterUpdate).toBe(1);
        expect(() => increment()).toThrow(TypeError);
        expect(nonWritableGlobalAfterSetterUpdate).toBe(1);
    });

    test("global variable in sloppy mode", () => {
        Object.defineProperty(globalThis, "nonWritableGlobalAfterSetterSloppy", {
            configurable: true,
            set(value) {
                Object.defineProperty(globalThis, "nonWritableGlobalAfterSetterSloppy", {
                    value: 1,
                    configurable: true,
                });
            },
        });
        function put(value) {
            nonWritableGlobalAfterSetterSloppy = value;
        }
        put(5);
        expect(nonWritableGlobalAfterSetterSloppy).toBe(1);
        put(9);
        expect(nonWritableGlobalAfterSetterSloppy).toBe(1);
    });
});
