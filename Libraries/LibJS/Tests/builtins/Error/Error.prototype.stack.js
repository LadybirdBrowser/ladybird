const stackDescriptor = Object.getOwnPropertyDescriptor(Error.prototype, "stack");
const stackGetter = stackDescriptor.get;
const stackSetter = stackDescriptor.set;

describe("getter - normal behavior", () => {
    test("basic functionality", () => {
        const stackFrames = [
            /^    at .*Error$/,
            /^    at .+\/Error\/Error\.prototype\.stack\.js:\d+:\d+$/,
            /^    at test \(.+\/test-common.js:\d+:\d+\)$/,
            /^    at .+\/Error\/Error\.prototype\.stack\.js:6:9$/,
            /^    at describe \(.+\/test-common\.js:\d+:\d+\)$/,
            /^    at .+\/Error\/Error\.prototype\.stack\.js:5:9$/,
        ];
        const values = [
            {
                error: new Error(),
                header: "Error",
                stackFrames,
            },
            {
                error: new TypeError(),
                header: "TypeError",
                stackFrames,
            },
            {
                error: new Error("Something went wrong!"),
                header: "Error: Something went wrong!",
                stackFrames,
            },
        ];

        for (const { error, header: expectedHeader, stackFrames: expectedStackFrames } of values) {
            const [header, ...stackFrames] = error.stack.trim().split("\n");

            expect(header).toBe(expectedHeader);
            expect(stackFrames).toHaveLength(expectedStackFrames.length);
            for (let i = 0; i < stackFrames.length; ++i) {
                const stackFrame = stackFrames[i];
                const expectedStackFrame = expectedStackFrames[i];
                expect(!!stackFrame.match(expectedStackFrame)).toBeTrue();
            }
        }
    });

    test("this value must be an object", () => {
        expect(() => {
            stackGetter.call("foo");
        }).toThrowWithMessage(TypeError, "foo is not an object");
        expect(() => {
            stackGetter.call(42);
        }).toThrowWithMessage(TypeError, "42 is not an object");
    });

    test("returns undefined when called with non-Error object this value", () => {
        expect(stackGetter.call({})).toBeUndefined();
    });
});

describe("setter - normal behavior", () => {
    test("basic functionality", () => {
        "use strict";
        const error = new Error();
        expect(Object.hasOwn(error, "stack")).toBeFalse();
        expect(() => {
            error.stack = "hello world";
        }).not.toThrowWithMessage(TypeError, "Cannot set property 'stack' of [object Error]");
        expect(Object.hasOwn(error, "stack")).toBeTrue();
        expect(error.stack).toBe("hello world");
    });

    test("the name of the set function is 'set stack'", () => {
        expect(stackSetter.name).toBe("set stack");
    });

    test("works on any object", () => {
        const nonError = {};
        expect(Object.hasOwn(nonError, "stack")).toBeFalse();
        stackSetter.call(nonError, "hello world");
        expect(Object.hasOwn(nonError, "stack")).toBeTrue();
        expect(nonError.stack).toBe("hello world");
    });

    test("accepts any value", () => {
        const nonError = { stack: 1 };
        stackSetter.call(nonError, undefined);
        expect(nonError.stack).toBeUndefined();
        stackSetter.call(nonError, null);
        expect(nonError.stack).toBeNull();
        stackSetter.call(nonError, NaN);
        expect(nonError.stack).toBeNaN();
        stackSetter.call(nonError, 1n);
        expect(nonError.stack).toBe(1n);
    });
});

describe("setter - errors", () => {
    test("this is not an object", () => {
        expect(() => {
            stackSetter.call(undefined);
        }).toThrowWithMessage(TypeError, "undefined is not an object");

        expect(() => {
            stackSetter.call(null);
        }).toThrowWithMessage(TypeError, "null is not an object");

        expect(() => {
            stackSetter.call(1);
        }).toThrowWithMessage(TypeError, "1 is not an object");

        expect(() => {
            stackSetter.call("foo");
        }).toThrowWithMessage(TypeError, "foo is not an object");
    });

    test("requires one argument", () => {
        expect(() => {
            stackSetter.call({});
        }).toThrowWithMessage(TypeError, "set stack() needs one argument");
    });

    test("create_data_property_or_throw can throw", () => {
        const revocable = Proxy.revocable([], {});
        revocable.revoke();
        expect(() => {
            stackSetter.call(revocable.proxy, "foo");
        }).toThrowWithMessage(TypeError, "An operation was performed on a revoked Proxy object");
    });
});
