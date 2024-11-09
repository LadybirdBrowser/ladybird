var describe;
var test;
var expect;
var withinSameSecond;

// Stores the results of each test and suite. Has a terrible
// name to avoid name collision.
var __TestResults__ = {};

// So test names like "toString" don't automatically produce an error
Object.setPrototypeOf(__TestResults__, null);

// This array is used to communicate with the C++ program. It treats
// each message in this array as a separate message. Has a terrible
// name to avoid name collision.
var __UserOutput__ = [];

// We also rebind console.log here to use the array above
console.log = (...args) => {
    __UserOutput__.push(args.join(" "));
};

class ExpectationError extends Error {
    constructor(message) {
        super(message);
        this.name = "ExpectationError";
    }
}

// Use an IIFE to avoid polluting the global namespace as much as possible
(() => {
    const deepEquals = (a, b) => {
        if (Object.is(a, b)) return true; // Handles identical references and primitives
        if ((a !== null && b === null) || (a === null && b !== null)) return false;
        if (Array.isArray(a)) return Array.isArray(b) && deepArrayEquals(a, b);
        if (typeof a === "object") return typeof b === "object" && deepObjectEquals(a, b);
    };

    const deepArrayEquals = (a, b) => {
        if (a.length !== b.length) return false;
        for (let i = 0; i < a.length; ++i) {
            if (!deepEquals(a[i], b[i])) return false;
        }

        return true;
    };

    const deepObjectEquals = (a, b) => {
        const keysA = Reflect.ownKeys(a);
        const keysB = Reflect.ownKeys(b);

        if (keysA.length !== keysB.length) return false;
        for (let key of keysA) {
            if (!deepEquals(a[key], b[key])) return false;
        }

        return true;
    };

    const valueToString = value => {
        try {
            if (value === 0 && 1 / value < 0) {
                return "-0";
            }
            return String(value);
        } catch {
            // e.g for objects without a prototype, the above throws.
            return Object.prototype.toString.call(value);
        }
    };

    class Expector {
        constructor(target, inverted) {
            this.target = target;
            this.inverted = !!inverted;
        }

        get not() {
            return new Expector(this.target, !this.inverted);
        }

        toBe(value) {
            this.__doMatcher(() => {
                this.__expect(
                    Object.is(this.target, value),
                    () =>
                        `toBe: expected _${valueToString(value)}_, got _${valueToString(
                            this.target
                        )}_`
                );
            });
        }

        toBeCloseTo(value, precision = 5) {
            this.__expect(
                typeof this.target === "number",
                () => `toBeCloseTo: expected target of type number, got ${typeof this.target}`
            );
            this.__expect(
                typeof value === "number",
                () => `toBeCloseTo: expected argument of type number, got ${typeof value}`
            );
            this.__expect(
                typeof precision === "number",
                () => `toBeCloseTo: expected precision of type number, got ${typeof precision}`
            );

            const epsilon = 10 ** -precision / 2;
            this.__doMatcher(() => {
                this.__expect(Math.abs(this.target - value) < epsilon);
            });
        }

        toHaveLength(length) {
            this.__expect(
                typeof this.target.length === "number",
                () => "toHaveLength: target.length not of type number"
            );

            this.__doMatcher(() => {
                this.__expect(Object.is(this.target.length, length));
            });
        }

        toHaveSize(size) {
            this.__expect(
                typeof this.target.size === "number",
                () => "toHaveSize: target.size not of type number"
            );

            this.__doMatcher(() => {
                this.__expect(Object.is(this.target.size, size));
            });
        }

        toHaveProperty(property, value) {
            this.__doMatcher(() => {
                let object = this.target;

                if (typeof property === "string" && property.includes(".")) {
                    let propertyArray = [];

                    while (property.includes(".")) {
                        let index = property.indexOf(".");
                        propertyArray.push(property.substring(0, index));
                        if (index + 1 >= property.length) break;
                        property = property.substring(index + 1, property.length);
                    }

                    propertyArray.push(property);

                    property = propertyArray;
                }

                if (Array.isArray(property)) {
                    for (let key of property) {
                        this.__expect(
                            object !== undefined && object !== null,
                            "got undefined or null as array key"
                        );
                        object = object[key];
                    }
                } else {
                    object = object[property];
                }

                this.__expect(object !== undefined, "should not be undefined");
                if (value !== undefined)
                    this.__expect(
                        deepEquals(object, value),
                        `value does not equal property ${valueToString(object)} vs ${valueToString(
                            value
                        )}`
                    );
            });
        }

        toBeDefined() {
            this.__doMatcher(() => {
                this.__expect(
                    this.target !== undefined,
                    () => "toBeDefined: expected target to be defined, got undefined"
                );
            });
        }

        toBeInstanceOf(class_) {
            this.__doMatcher(() => {
                this.__expect(
                    this.target instanceof class_,
                    `Expected ${valueToString(this.target)} to be instance of ${class_.name}`
                );
            });
        }

        toBeNull() {
            this.__doMatcher(() => {
                this.__expect(
                    this.target === null,
                    `Expected target to be null got ${valueToString(this.target)}`
                );
            });
        }

        toBeUndefined() {
            this.__doMatcher(() => {
                this.__expect(
                    this.target === undefined,
                    () =>
                        `toBeUndefined: expected target to be undefined, got _${valueToString(
                            this.target
                        )}_`
                );
            });
        }

        toBeNaN() {
            this.__doMatcher(() => {
                this.__expect(
                    isNaN(this.target),
                    () => `toBeNaN: expected target to be NaN, got _${valueToString(this.target)}_`
                );
            });
        }

        toBeTrue(customDetails = undefined) {
            this.__doMatcher(() => {
                this.__expect(
                    this.target === true,
                    () =>
                        `toBeTrue: expected target to be true, got _${valueToString(this.target)}_${
                            customDetails ? ` (${customDetails})` : ""
                        }`
                );
            });
        }

        toBeFalse(customDetails = undefined) {
            this.__doMatcher(() => {
                this.__expect(
                    this.target === false,
                    () =>
                        `toBeFalse: expected target to be false, got _${valueToString(
                            this.target
                        )}_${customDetails ?? ""}`
                );
            });
        }

        __validateNumericComparisonTypes(value) {
            this.__expect(typeof this.target === "number" || typeof this.target === "bigint");
            this.__expect(typeof value === "number" || typeof value === "bigint");
            this.__expect(typeof this.target === typeof value);
        }

        toBeLessThan(value) {
            this.__validateNumericComparisonTypes(value);

            this.__doMatcher(() => {
                this.__expect(this.target < value);
            });
        }

        toBeLessThanOrEqual(value) {
            this.__validateNumericComparisonTypes(value);

            this.__doMatcher(() => {
                this.__expect(this.target <= value);
            });
        }

        toBeGreaterThan(value) {
            this.__validateNumericComparisonTypes(value);

            this.__doMatcher(() => {
                this.__expect(this.target > value);
            });
        }

        toBeGreaterThanOrEqual(value) {
            this.__validateNumericComparisonTypes(value);

            this.__doMatcher(() => {
                this.__expect(this.target >= value);
            });
        }

        toContain(item) {
            this.__doMatcher(() => {
                for (let element of this.target) {
                    if (item === element) return;
                }

                throw new ExpectationError();
            });
        }

        toContainEqual(item) {
            this.__doMatcher(() => {
                for (let element of this.target) {
                    if (deepEquals(item, element)) return;
                }

                throw new ExpectationError();
            });
        }

        toEqual(value) {
            this.__doMatcher(() => {
                this.__expect(
                    deepEquals(this.target, value),
                    () =>
                        `Expected _${valueToString(value)}_, but got _${valueToString(
                            this.target
                        )}_`
                );
            });
        }

        toThrow(value) {
            this.__expect(typeof this.target === "function");
            this.__expect(
                typeof value === "string" ||
                    typeof value === "function" ||
                    typeof value === "object" ||
                    value === undefined
            );

            this.__doMatcher(() => {
                let threw = true;
                try {
                    this.target();
                    threw = false;
                } catch (e) {
                    if (typeof value === "string") {
                        this.__expect(
                            e.message.includes(value),
                            `Expected ${this.target.toString()} to throw and message to include "${value}" but message "${
                                e.message
                            }" did not contain it`
                        );
                    } else if (typeof value === "function") {
                        this.__expect(
                            e instanceof value,
                            `Expected ${this.target.toString()} to throw and be of type ${value} but it threw ${e}`
                        );
                    } else if (typeof value === "object") {
                        this.__expect(
                            e.message === value.message,
                            `Expected ${this.target.toString()} to throw and message to be ${value} but it threw with message ${
                                e.message
                            }`
                        );
                    }
                }
                this.__expect(
                    threw,
                    `Expected ${this.target.toString()} to throw but it didn't throw anything`
                );
            });
        }

        pass(message) {
            // FIXME: This does nothing. If we want to implement things
            // like assertion count, this will have to do something
        }

        // jest-extended
        fail(message) {
            this.__doMatcher(() => {
                this.__expect(false, message);
            });
        }

        // jest-extended
        toThrowWithMessage(class_, message) {
            this.__expect(typeof this.target === "function");
            this.__expect(class_ !== undefined);
            this.__expect(message !== undefined);

            this.__doMatcher(() => {
                try {
                    this.target();
                    this.__expect(false, () => "toThrowWithMessage: target function did not throw");
                } catch (e) {
                    this.__expect(
                        e instanceof class_,
                        () =>
                            `toThrowWithMessage: expected error to be instance of ${valueToString(
                                class_.name
                            )}, got ${valueToString(e.name)}`
                    );
                    this.__expect(
                        e.message.includes(message),
                        () =>
                            `toThrowWithMessage: expected error message to include _${valueToString(
                                message
                            )}_, got _${valueToString(e.message)}_`
                    );
                }
            });
        }

        // Test for syntax errors; target must be a string
        toEval() {
            this.__expect(typeof this.target === "string");
            const success = canParseSource(this.target);
            this.__expect(
                this.inverted ? !success : success,
                () =>
                    `Expected _${valueToString(this.target)}_ ` +
                    (this.inverted ? "not to eval but it did" : "to eval but it didn't")
            );
        }

        // Must compile regardless of inverted-ness
        toEvalTo(value) {
            this.__expect(typeof this.target === "string");

            let result;

            try {
                result = eval(this.target);
            } catch (e) {
                throw new ExpectationError(
                    `Expected _${valueToString(this.target)}_ to eval but it failed with ${e}`
                );
            }

            this.__doMatcher(() => {
                this.__expect(
                    deepEquals(value, result),
                    () =>
                        `Expected _${valueToString(this.target)}_ to eval to ` +
                        `_${valueToString(value)}_ but got _${valueToString(result)}_`
                );
            });
        }

        toHaveConfigurableProperty(property) {
            this.__expect(this.target !== undefined && this.target !== null);
            let d = Object.getOwnPropertyDescriptor(this.target, property);
            this.__expect(d !== undefined);

            this.__doMatcher(() => {
                this.__expect(d.configurable);
            });
        }

        toHaveEnumerableProperty(property) {
            this.__expect(this.target !== undefined && this.target !== null);
            let d = Object.getOwnPropertyDescriptor(this.target, property);
            this.__expect(d !== undefined);

            this.__doMatcher(() => {
                this.__expect(d.enumerable);
            });
        }

        toHaveWritableProperty(property) {
            this.__expect(this.target !== undefined && this.target !== null);
            let d = Object.getOwnPropertyDescriptor(this.target, property);
            this.__expect(d !== undefined);

            this.__doMatcher(() => {
                this.__expect(d.writable);
            });
        }

        toHaveValueProperty(property, value) {
            this.__expect(this.target !== undefined && this.target !== null);
            let d = Object.getOwnPropertyDescriptor(this.target, property);
            this.__expect(d !== undefined);

            this.__doMatcher(() => {
                this.__expect(d.value !== undefined);
                if (value !== undefined) this.__expect(deepEquals(value, d.value));
            });
        }

        toHaveGetterProperty(property) {
            this.__expect(this.target !== undefined && this.target !== null);
            let d = Object.getOwnPropertyDescriptor(this.target, property);
            this.__expect(d !== undefined);

            this.__doMatcher(() => {
                this.__expect(d.get !== undefined);
            });
        }

        toHaveSetterProperty(property) {
            this.__expect(this.target !== undefined && this.target !== null);
            let d = Object.getOwnPropertyDescriptor(this.target, property);
            this.__expect(d !== undefined);

            this.__doMatcher(() => {
                this.__expect(d.set !== undefined);
            });
        }

        toBeIteratorResultWithValue(value) {
            this.__expect(this.target !== undefined && this.target !== null);
            this.__doMatcher(() => {
                this.__expect(
                    this.target.done === false,
                    () =>
                        `toGiveIteratorResultWithValue: expected 'done' to be _false_ got ${valueToString(
                            this.target.done
                        )}`
                );
                this.__expect(
                    deepEquals(value, this.target.value),
                    () =>
                        `toGiveIteratorResultWithValue: expected 'value' to be _${valueToString(
                            value
                        )}_ got ${valueToString(this.target.value)}`
                );
            });
        }

        toBeIteratorResultDone() {
            this.__expect(this.target !== undefined && this.target !== null);
            this.__doMatcher(() => {
                this.__expect(
                    this.target.done === true,
                    () =>
                        `toGiveIteratorResultDone: expected 'done' to be _true_ got ${valueToString(
                            this.target.done
                        )}`
                );
                this.__expect(
                    this.target.value === undefined,
                    () =>
                        `toGiveIteratorResultDone: expected 'value' to be _undefined_ got ${valueToString(
                            this.target.value
                        )}`
                );
            });
        }

        __doMatcher(matcher) {
            if (!this.inverted) {
                matcher();
            } else {
                let threw = false;
                try {
                    matcher();
                } catch (e) {
                    if (e.name === "ExpectationError") threw = true;
                }
                if (!threw) throw new ExpectationError("not: test didn't fail");
            }
        }

        __expect(value, details) {
            if (value !== true) {
                if (details !== undefined) {
                    if (details instanceof Function) throw new ExpectationError(details());
                    else throw new ExpectationError(details);
                } else {
                    throw new ExpectationError();
                }
            }
        }
    }

    expect = value => new Expector(value);

    // describe is able to lump test results inside of it by using this context
    // variable. Top level tests have the default suite message
    const defaultSuiteMessage = "__$$TOP_LEVEL$$__";
    let suiteMessage = defaultSuiteMessage;

    describe = (message, callback) => {
        suiteMessage = message;
        if (!__TestResults__[suiteMessage]) __TestResults__[suiteMessage] = {};
        try {
            callback();
        } catch (e) {
            __TestResults__[suiteMessage][defaultSuiteMessage] = {
                result: "fail",
                details: String(e),
                duration: 0,
            };
        }
        suiteMessage = defaultSuiteMessage;
    };

    test = (message, callback) => {
        if (!__TestResults__[suiteMessage]) __TestResults__[suiteMessage] = {};

        const suite = __TestResults__[suiteMessage];
        if (Object.prototype.hasOwnProperty.call(suite, message)) {
            suite[message] = {
                result: "fail",
                details: "Another test with the same message did already run",
                duration: 0,
            };
            return;
        }

        const now = () => Temporal.Now.instant().epochNanoseconds;
        const start = now();
        const time_us = () => Number(BigInt.asIntN(53, (now() - start) / 1000n));
        try {
            callback();
            suite[message] = {
                result: "pass",
                duration: time_us(),
            };
        } catch (e) {
            suite[message] = {
                result: "fail",
                details: String(e),
                duration: time_us(),
            };
        }
    };

    test.skip = (message, callback) => {
        if (typeof callback !== "function")
            throw new Error("test.skip has invalid second argument (must be a function)");

        if (!__TestResults__[suiteMessage]) __TestResults__[suiteMessage] = {};

        const suite = __TestResults__[suiteMessage];
        if (Object.prototype.hasOwnProperty.call(suite, message)) {
            suite[message] = {
                result: "fail",
                details: "Another test with the same message did already run",
                duration: 0,
            };
            return;
        }

        suite[message] = {
            result: "skip",
            duration: 0,
        };
    };

    test.xfail = (message, callback) => {
        if (!__TestResults__[suiteMessage]) __TestResults__[suiteMessage] = {};

        const suite = __TestResults__[suiteMessage];
        if (Object.prototype.hasOwnProperty.call(suite, message)) {
            suite[message] = {
                result: "fail",
                details: "Another test with the same message did already run",
                duration: 0,
            };
            return;
        }

        const now = () => Temporal.Now.instant().epochNanoseconds;
        const start = now();
        const time_us = () => Number(BigInt.asIntN(53, (now() - start) / 1000n));
        try {
            callback();
            suite[message] = {
                result: "fail",
                details: "Expected test to fail, but it passed",
                duration: time_us(),
            };
        } catch (e) {
            suite[message] = {
                result: "xfail",
                duration: time_us(),
            };
        }
    };

    test.xfailIf = (condition, message, callback) => {
        condition ? test.xfail(message, callback) : test(message, callback);
    };

    withinSameSecond = callback => {
        let callbackDuration;
        for (let tries = 0; tries < 5; tries++) {
            const start = Temporal.Now.instant();
            const result = callback();
            const end = Temporal.Now.instant();
            if (start.epochSeconds !== end.epochSeconds) {
                callbackDuration = start.until(end);
                continue;
            }
            return result;
        }
        throw new ExpectationError(
            `Tried to execute callback '${callback}' 5 times within the same second but ` +
                `failed. Make sure the callback does as little work as possible (the last run ` +
                `took ${callbackDuration.total(
                    "milliseconds"
                )} ms) and the machine is not overloaded. If you see this ` +
                `error appearing in the CI it is most likely a flaky failure!`
        );
    };
})();
