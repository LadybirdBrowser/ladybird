describe("errors", () => {
    test("first argument must be coercible to object", () => {
        expect(() => {
            new AggregateError();
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
    });

    test("@@iterator throws", () => {
        expect(() => {
            new AggregateError({
                [Symbol.iterator]() {
                    throw Error("oops!");
                },
            });
        }).toThrowWithMessage(Error, "oops!");
    });
});

describe("normal behavior", () => {
    test("length is 2", () => {
        expect(AggregateError).toHaveLength(2);
    });

    test("name is AggregateError", () => {
        expect(AggregateError.name).toBe("AggregateError");
    });

    test("Prototype of the AggregateError constructor is the Error constructor", () => {
        expect(Object.getPrototypeOf(AggregateError)).toBe(Error);
    });

    test("Prototype of AggregateError.prototype is Error.prototype", () => {
        expect(Object.getPrototypeOf(AggregateError.prototype)).toBe(Error.prototype);
    });

    test("basic functionality", () => {
        expect(AggregateError([])).toBeInstanceOf(AggregateError);
        expect(new AggregateError([])).toBeInstanceOf(AggregateError);
        expect(new AggregateError([]).toString()).toBe("AggregateError");
        expect(new AggregateError([], "Foo").toString()).toBe("AggregateError: Foo");
        expect(new AggregateError([]).hasOwnProperty("errors")).toBeTrue();
        const errors = [1, 2, 3];
        expect(new AggregateError(errors).errors).toEqual(errors);
        expect(new AggregateError(errors).errors).not.toBe(errors);
        expect(
            new AggregateError({
                [Symbol.iterator]: (i = 0) => ({
                    next() {
                        if (i < 3) {
                            i++;
                            return { value: i };
                        }
                        return { value: null, done: true };
                    },
                }),
            }).errors
        ).toEqual(errors);
    });

    test("supports options object with cause", () => {
        const cause = new Error();
        const error = new AggregateError([], "test", { cause });
        expect(error.hasOwnProperty("cause")).toBeTrue();
        expect(error.cause).toBe(cause);
    });
});
