// JIT code answers reads of properties that property caches remember as missing
// without calling into the runtime, for plain objects and for ECMAScript functions.
// These tests run every function many times, so that with a low JIT threshold they
// are compiled.

const iterations = 100;

function readDefaultProps(value) {
    return value.defaultProps;
}

function readPrototype(value) {
    return value.prototype;
}

describe("missing properties in JIT code", () => {
    test("plain objects", () => {
        const object = { a: 1 };
        for (let i = 0; i < iterations; ++i) expect(readDefaultProps(object)).toBeUndefined();
        const other = { a: 2 };
        other.defaultProps = "own";
        expect(readDefaultProps(other)).toBe("own");
        Object.prototype.defaultProps = "inherited";
        try {
            expect(readDefaultProps(object)).toBe("inherited");
        } finally {
            delete Object.prototype.defaultProps;
        }
        expect(readDefaultProps(object)).toBeUndefined();
    });

    test("functions", () => {
        function component() {}
        for (let i = 0; i < iterations; ++i) expect(readDefaultProps(component)).toBeUndefined();
        Function.prototype.defaultProps = "inherited";
        try {
            expect(readDefaultProps(component)).toBe("inherited");
        } finally {
            delete Function.prototype.defaultProps;
        }
        component.defaultProps = { a: 1 };
        expect(readDefaultProps(component)).toEqual({ a: 1 });
    });

    test("prototypes that functions create lazily", () => {
        const method = { method() {} }.method;
        for (let i = 0; i < iterations; ++i) {
            expect(readPrototype(method)).toBeUndefined();
            const lazy = function () {};
            expect(readPrototype(lazy).constructor).toBe(lazy);
        }
    });
});
