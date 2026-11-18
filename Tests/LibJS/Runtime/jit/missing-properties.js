// JIT code answers reads of properties that property caches remember as missing
// without calling into the runtime, for objects that have no other properties than
// those of their shape.
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

    test("objects of other classes", () => {
        const set = new Set();
        const iterator = [].values();
        for (let i = 0; i < iterations; ++i) {
            expect(readDefaultProps(set)).toBeUndefined();
            expect(readDefaultProps(iterator)).toBeUndefined();
        }
        Set.prototype.defaultProps = "inherited";
        try {
            expect(readDefaultProps(set)).toBe("inherited");
            expect(readDefaultProps(iterator)).toBeUndefined();
        } finally {
            delete Set.prototype.defaultProps;
        }
        expect(readDefaultProps(set)).toBeUndefined();
    });

    test("a proxy with the shape of an object that the property is missing from", () => {
        function read(value) {
            return value.defaultProps;
        }
        // This object and the proxy have the same shape: no properties, and
        // Object.prototype as a prototype that the empty shape was given later.
        const plain = Object.setPrototypeOf(Object.create(null), Object.prototype);
        const proxy = new Proxy({}, { get: (target, key) => `trap ${key}` });
        for (let i = 0; i < iterations; ++i) {
            expect(read(plain)).toBeUndefined();
            expect(read(proxy)).toBe("trap defaultProps");
        }
    });
});
