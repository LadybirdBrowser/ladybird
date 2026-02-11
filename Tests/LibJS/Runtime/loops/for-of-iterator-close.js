function makeIterator(values) {
    let returnCalled = false;
    let returnArgument = undefined;
    let index = 0;
    const iterator = {
        [Symbol.iterator]() {
            return {
                next() {
                    if (index < values.length) return { value: values[index++], done: false };
                    return { done: true };
                },
                return(value) {
                    returnCalled = true;
                    returnArgument = value;
                    return { done: true };
                },
            };
        },
        get returnCalled() {
            return returnCalled;
        },
        get returnArgument() {
            return returnArgument;
        },
    };
    return iterator;
}

describe("for-of iterator close on abrupt completion", () => {
    test("break calls return()", () => {
        const iter = makeIterator([1, 2, 3]);
        for (const x of iter) {
            if (x === 2) break;
        }
        expect(iter.returnCalled).toBeTrue();
    });

    test("return from function calls return()", () => {
        const iter = makeIterator([1, 2, 3]);
        (function () {
            for (const x of iter) {
                if (x === 1) return;
            }
        })();
        expect(iter.returnCalled).toBeTrue();
    });

    test("throw from body calls return()", () => {
        const iter = makeIterator([1, 2, 3]);
        try {
            for (const x of iter) {
                throw new Error("oops");
            }
        } catch {}
        expect(iter.returnCalled).toBeTrue();
    });

    test("continue to same loop does NOT call return()", () => {
        const iter = makeIterator([1, 2, 3]);
        for (const x of iter) {
            continue;
        }
        expect(iter.returnCalled).toBeFalse();
    });

    test("continue to outer label calls return()", () => {
        const iter = makeIterator([1, 2, 3]);
        outer: for (let i = 0; i < 1; i++) {
            for (const x of iter) {
                continue outer;
            }
        }
        expect(iter.returnCalled).toBeTrue();
    });

    test("normal completion does NOT call return()", () => {
        const iter = makeIterator([1, 2, 3]);
        for (const x of iter) {
            // exhaust normally
        }
        expect(iter.returnCalled).toBeFalse();
    });

    test("iterator without return() method: no error on break", () => {
        let index = 0;
        const iter = {
            [Symbol.iterator]() {
                return {
                    next() {
                        if (index < 3) return { value: index++, done: false };
                        return { done: true };
                    },
                    // no return() method
                };
            },
        };
        expect(() => {
            for (const x of iter) {
                break;
            }
        }).not.toThrow();
    });

    test("return() that throws during non-throw exit: close error propagates", () => {
        const iter = {
            [Symbol.iterator]() {
                return {
                    next() {
                        return { value: 1, done: false };
                    },
                    return() {
                        throw new Error("close error");
                    },
                };
            },
        };
        expect(() => {
            for (const x of iter) {
                break;
            }
        }).toThrowWithMessage(Error, "close error");
    });

    test("return() that throws during throw exit: original throw takes precedence", () => {
        const iter = {
            [Symbol.iterator]() {
                return {
                    next() {
                        return { value: 1, done: false };
                    },
                    return() {
                        throw new Error("close error");
                    },
                };
            },
        };
        expect(() => {
            for (const x of iter) {
                throw new Error("body error");
            }
        }).toThrowWithMessage(Error, "body error");
    });

    test("LHS destructuring that throws calls return()", () => {
        let returnCalled = false;
        const iter = {
            [Symbol.iterator]() {
                return {
                    next() {
                        return { value: 42, done: false };
                    },
                    return() {
                        returnCalled = true;
                        return { done: true };
                    },
                };
            },
        };
        try {
            for (const [a, b] of iter) {
                // destructuring a non-iterable (42) should throw
            }
        } catch {}
        expect(returnCalled).toBeTrue();
    });

    test("break from for-of inside try/finally: both finally and return() run", () => {
        const events = [];
        const iter = {
            [Symbol.iterator]() {
                return {
                    next() {
                        return { value: 1, done: false };
                    },
                    return() {
                        events.push("return");
                        return { done: true };
                    },
                };
            },
        };
        try {
            for (const x of iter) {
                break;
            }
        } finally {
            events.push("finally");
        }
        expect(events).toEqual(["return", "finally"]);
    });

    test("for-of with var binding and break", () => {
        const iter = makeIterator([1, 2, 3]);
        for (var x of iter) {
            if (x === 2) break;
        }
        expect(x).toBe(2);
        expect(iter.returnCalled).toBeTrue();
    });

    test("for-of with let binding and break", () => {
        const iter = makeIterator([10, 20, 30]);
        let last;
        for (let x of iter) {
            last = x;
            if (x === 20) break;
        }
        expect(last).toBe(20);
        expect(iter.returnCalled).toBeTrue();
    });

    test("nested for-of: break from inner closes inner, not outer", () => {
        const outerIter = makeIterator([1, 2]);
        const innerIter = makeIterator(["a", "b", "c"]);
        const results = [];
        for (const x of outerIter) {
            for (const y of innerIter) {
                results.push(`${x}${y}`);
                if (y === "a") break;
            }
        }
        expect(innerIter.returnCalled).toBeTrue();
        // outerIter is exhausted normally (only 2 elements)
        expect(outerIter.returnCalled).toBeFalse();
    });

    test("break to outer label from nested for-of closes both", () => {
        let outerReturnCalled = false;
        let innerReturnCalled = false;

        const outerIter = {
            [Symbol.iterator]() {
                return {
                    i: 0,
                    next() {
                        return this.i++ < 3 ? { value: this.i, done: false } : { done: true };
                    },
                    return() {
                        outerReturnCalled = true;
                        return { done: true };
                    },
                };
            },
        };

        const innerIter = {
            [Symbol.iterator]() {
                return {
                    i: 0,
                    next() {
                        return this.i++ < 3 ? { value: this.i, done: false } : { done: true };
                    },
                    return() {
                        innerReturnCalled = true;
                        return { done: true };
                    },
                };
            },
        };

        outer: for (const x of outerIter) {
            for (const y of innerIter) {
                break outer;
            }
        }

        expect(innerReturnCalled).toBeTrue();
        expect(outerReturnCalled).toBeTrue();
    });
});

describe("for-await-of iterator close on abrupt completion", () => {
    test("break calls return()", () => {
        let returnCalled = false;
        const asyncIter = {
            [Symbol.asyncIterator]() {
                return {
                    i: 0,
                    next() {
                        return Promise.resolve(this.i++ < 3 ? { value: this.i, done: false } : { done: true });
                    },
                    return() {
                        returnCalled = true;
                        return Promise.resolve({ done: true });
                    },
                };
            },
        };

        const promise = (async () => {
            for await (const x of asyncIter) {
                break;
            }
        })();

        return promise.then(() => {
            expect(returnCalled).toBeTrue();
        });
    });

    test("throw from body calls return()", () => {
        let returnCalled = false;
        const asyncIter = {
            [Symbol.asyncIterator]() {
                return {
                    next() {
                        return Promise.resolve({ value: 1, done: false });
                    },
                    return() {
                        returnCalled = true;
                        return Promise.resolve({ done: true });
                    },
                };
            },
        };

        const promise = (async () => {
            for await (const x of asyncIter) {
                throw new Error("oops");
            }
        })();

        return promise.catch(() => {
            expect(returnCalled).toBeTrue();
        });
    });

    test("sync iterator with rejected promise value: return() called only once", () => {
        let returnCount = 0;
        const syncIter = {
            [Symbol.iterator]() {
                return {
                    next() {
                        return { value: Promise.reject("reject"), done: false };
                    },
                    return() {
                        returnCount += 1;
                    },
                };
            },
        };

        const promise = (async () => {
            for await (const x of syncIter) {
                // The rejected promise causes the async-from-sync iterator
                // to call return() internally. Our for-await-of close logic
                // must not double-close.
            }
        })();

        return promise.catch(() => {
            expect(returnCount).toBe(1);
        });
    });
});
