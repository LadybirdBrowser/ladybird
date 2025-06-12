const sentinel = "whf :^)";

describe("errors", () => {
    test("setter called on non-object", () => {
        let { get, set } = Object.getOwnPropertyDescriptor(Iterator.prototype, "constructor");

        expect(() => {
            set.call(undefined, sentinel);
        }).toThrowWithMessage(TypeError, "undefined is not an object");
    });

    test("cannot set the built-in Iterator's constructor", () => {
        expect(() => {
            Iterator.prototype.constructor = sentinel;
        }).toThrowWithMessage(
            TypeError,
            "Cannot write to non-writable property '[object IteratorPrototype]'"
        );
    });
});

describe("correct behavior", () => {
    test("basic functionality", () => {
        expect(Iterator.prototype.constructor).toBe(Iterator);
    });

    test("constructor setter", () => {
        let Proto = Object.create(Iterator.prototype);
        Proto.constructor = sentinel;

        expect(Proto.constructor).toBe(sentinel);
    });
});
