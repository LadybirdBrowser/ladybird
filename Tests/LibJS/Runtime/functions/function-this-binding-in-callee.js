test("sloppy functions bind null and undefined to the global this value", () => {
    function sloppy() {
        return this;
    }
    for (let i = 0; i < 100; ++i) {
        expect(sloppy()).toBe(globalThis);
        expect(sloppy.call(null)).toBe(globalThis);
        expect(sloppy.call(undefined)).toBe(globalThis);
    }
});

test("sloppy functions box primitive this values", () => {
    function sloppy() {
        return this;
    }
    const object = { sloppy };
    for (let i = 0; i < 100; ++i) {
        const boxed = sloppy.call(5);
        expect(typeof boxed).toBe("object");
        expect(boxed.valueOf()).toBe(5);
        expect(sloppy.call("text") instanceof String).toBeTrue();
        expect(object.sloppy()).toBe(object);
    }
    Number.prototype.sloppy = sloppy;
    try {
        for (let i = 0; i < 100; ++i) {
            const boxed = (7).sloppy();
            expect(typeof boxed).toBe("object");
            expect(boxed.valueOf()).toBe(7);
        }
    } finally {
        delete Number.prototype.sloppy;
    }
});

test("strict functions get the this value as it is", () => {
    function strict() {
        "use strict";
        return this;
    }
    for (let i = 0; i < 100; ++i) {
        expect(strict()).toBeUndefined();
        expect(strict.call(null)).toBeNull();
        expect(strict.call(5)).toBe(5);
    }
});

test("sloppy functions with parameter expressions bind their this value", () => {
    function sloppy(a = 1, read = () => a) {
        var a = 2;
        return [this, read(), a];
    }
    for (let i = 0; i < 100; ++i) {
        expect(sloppy()).toEqual([globalThis, 1, 2]);
        const [boxed] = sloppy.call(true);
        expect(boxed instanceof Boolean).toBeTrue();
    }
});

test("sloppy getters on primitives get a boxed this value", () => {
    Object.defineProperty(String.prototype, "sloppyThis", {
        get() {
            return this;
        },
        configurable: true,
    });
    try {
        for (let i = 0; i < 100; ++i) expect(typeof "abc".sloppyThis).toBe("object");
    } finally {
        delete String.prototype.sloppyThis;
    }
});
