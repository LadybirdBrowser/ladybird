// JIT code adds properties to objects it allocated directly, also through
// property caches that saw several shapes. These tests run every function
// many times, so that with a low JIT threshold they are compiled.

const iterations = 100;

describe("property additions to allocated objects", () => {
    test("a helper adding a property to objects of several shapes", () => {
        function setName(object, name) {
            object.name = name;
        }
        function makeA(i) {
            const object = { a: i };
            setName(object, "a");
            return object;
        }
        function makeB(i) {
            const object = { b: i, c: 2 };
            setName(object, "bb");
            return object.b + object.c + object.name.length;
        }
        for (let i = 0; i < iterations; ++i) {
            const a = makeA(i);
            expect(Object.keys(a)).toEqual(["a", "name"]);
            expect(a.name).toBe("a");
            expect(makeB(i)).toBe(i + 4);
        }
    });

    test("a helper adding a property or writing an existing one", () => {
        function setValue(object, value) {
            object.value = value;
        }
        function fresh(i) {
            const object = {};
            setValue(object, i);
            return object;
        }
        function existing(i) {
            const object = { value: 0 };
            setValue(object, i);
            return object;
        }
        for (let i = 0; i < iterations; ++i) {
            expect(fresh(i)).toEqual({ value: i });
            expect(existing(i)).toEqual({ value: i });
        }
    });

    test("objects other code can see are not assumed unchanged", () => {
        function mutate(object) {
            for (const key of Object.keys(object)) object[key] = key;
            object.extra = true;
        }
        function passed(i) {
            const object = { a: i };
            mutate(object);
            return [object.a, object.extra];
        }
        function throughPhi(i, other) {
            const object = { a: i };
            const either = i % 2 ? object : other;
            mutate(either);
            return object.a;
        }
        for (let i = 0; i < iterations; ++i) {
            expect(passed(i)).toEqual(["a", true]);
            expect(throughPhi(i, {})).toBe(i % 2 ? "a" : i);
        }
    });

    test("exits after additions", () => {
        function setFlag(object) {
            object.flag = true;
        }
        function build(i, reveal) {
            const object = i % 2 ? { odd: i } : { even: i, extra: 0 };
            setFlag(object);
            if (reveal) return reveal(object);
            return object.flag;
        }
        for (let i = 0; i < iterations; ++i) expect(build(i, null)).toBeTrue();
        expect(build(3, object => object)).toEqual({ odd: 3, flag: true });
        expect(build(4, object => object)).toEqual({ even: 4, extra: 0, flag: true });
    });
});

describe("property additions after preventing extensions", () => {
    const lockers = {
        preventExtensions: object => Object.preventExtensions(object),
        reflectPreventExtensions: object => Reflect.preventExtensions(object),
        seal: object => Object.seal(object),
        freeze: object => Object.freeze(object),
    };
    function gatedLockers(lock) {
        const state = { armed: false };
        // Keep the call polymorphic so it is not inlined and its cold locking branch cannot exit the caller.
        const callbacks = [
            object => {
                if (state.armed) lock(object);
            },
            object => {
                if (state.armed) lock(object);
            },
        ];
        return { state, callbacks };
    }

    for (const [name, lock] of Object.entries(lockers)) {
        for (const strict of [false, true]) {
            const directive = strict ? "'use strict';" : "";
            const mode = strict ? "strict" : "sloppy";

            test(`${mode} object literal: ${name}`, () => {
                const { state, callbacks } = gatedLockers(lock);
                const make = new Function(
                    "lock",
                    "p",
                    `${directive} const object = {}; if (p) lock(object); object.x = 1; return object;`
                );
                jit.prepare(make);
                for (let i = 0; i < 50; ++i) make(callbacks[i % 2], true);
                expect(jit.compile(make)).toBe(jit.enabled);

                state.armed = true;
                if (strict) {
                    expect(() => make(callbacks[0], true)).toThrow(TypeError);
                } else {
                    const object = make(callbacks[0], true);
                    expect(Object.hasOwn(object, "x")).toBeFalse();
                    expect(Object.isExtensible(object)).toBeFalse();
                }
                expect(make(lock, false).x).toBe(1);
            });

            test(`${mode} inlined constructor: ${name}`, () => {
                const { state, callbacks } = gatedLockers(lock);
                const Constructor = new Function("lock", "p", `${directive} if (p) lock(this); this.x = 1;`);
                const make = new Function("Constructor", "lock", "p", "return new Constructor(lock, p);");
                jit.prepare(make);
                for (let i = 0; i < 50; ++i) make(Constructor, callbacks[i % 2], true);
                expect(jit.compile(make)).toBe(jit.enabled);

                state.armed = true;
                if (strict) {
                    expect(() => make(Constructor, callbacks[0], true)).toThrow(TypeError);
                } else {
                    const object = make(Constructor, callbacks[0], true);
                    expect(Object.hasOwn(object, "x")).toBeFalse();
                    expect(Object.isExtensible(object)).toBeFalse();
                }
                expect(make(Constructor, lock, false).x).toBe(1);
            });
        }
    }
});
