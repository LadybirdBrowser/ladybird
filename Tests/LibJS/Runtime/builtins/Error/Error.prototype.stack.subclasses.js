function framesOf(error) {
    return error.stack
        .split("\n")
        .filter(line => line.startsWith("    at "))
        .map(line => line.trim().split(" ")[1]);
}

test("the constructors of error subclasses are left out", () => {
    class MyError extends Error {
        constructor(message) {
            super(message);
        }
    }
    class DerivedError extends MyError {
        constructor() {
            super("derived");
        }
    }
    class ImplicitError extends TypeError {}
    function make(ErrorClass) {
        return new ErrorClass([]);
    }
    for (const ErrorClass of [MyError, DerivedError, ImplicitError, Error, TypeError, AggregateError])
        expect(framesOf(make(ErrorClass))[0]).toBe("make");
});

test("calling an error constructor leaves out only its own frame", () => {
    function call() {
        return Error("called");
    }
    function callType() {
        return RangeError("called");
    }
    expect(framesOf(call())[0]).toBe("call");
    expect(framesOf(callType())[0]).toBe("callType");
});

test("a new.target with no frame on the stack leaves out only the frame of the constructor", () => {
    class Unrelated extends Error {}
    function reflect() {
        return Reflect.construct(TypeError, ["reflected"], Unrelated);
    }
    // NB: The frame of Reflect.construct() itself is not left out.
    expect(framesOf(reflect()).slice(0, 2)).toEqual(["<unknown>", "reflect"]);
});

test("a function constructor that makes an error for its new.target is left out", () => {
    function LegacyError(message) {
        return Reflect.construct(Error, [message], new.target);
    }
    LegacyError.prototype = Object.create(Error.prototype);
    function make() {
        return new LegacyError("legacy");
    }
    expect(framesOf(make())[0]).toBe("make");
});

test("a recursive constructor leaves out its innermost frame only", () => {
    class Nested extends Error {
        constructor(depth) {
            if (depth > 0) {
                super(String(new Nested(depth - 1).stack));
            } else {
                super("innermost");
            }
        }
    }
    const inner = new Nested(1).message;
    const frames = inner
        .split("\n")
        .filter(line => line.startsWith("    at "))
        .map(line => line.trim().split(" ")[1]);
    expect(frames[0]).toBe("Nested");
});
