const sentinel = "whf :^)";

describe("errors", () => {
    test("setter called on non-object", () => {
        let { get, set } = Object.getOwnPropertyDescriptor(Iterator.prototype, Symbol.toStringTag);

        expect(() => {
            set.call(undefined, sentinel);
        }).toThrowWithMessage(TypeError, "undefined is not an object");
    });

    test("cannot set the built-in Iterator's toStringTag", () => {
        expect(() => {
            Iterator.prototype[Symbol.toStringTag] = sentinel;
        }).toThrowWithMessage(
            TypeError,
            "Cannot write to non-writable property '[object IteratorPrototype]'"
        );
    });
});

describe("correct behavior", () => {
    test("basic functionality", () => {
        expect(Iterator.prototype[Symbol.toStringTag]).toBe("Iterator");
    });

    test("toStringTag setter", () => {
        let Proto = Object.create(Iterator.prototype);
        Proto[Symbol.toStringTag] = sentinel;

        expect(Proto[Symbol.toStringTag]).toBe(sentinel);
    });
});
