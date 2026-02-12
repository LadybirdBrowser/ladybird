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
    function makeAsyncIterator(values) {
        let returnCalled = false;
        return {
            [Symbol.asyncIterator]() {
                let index = 0;
                return {
                    next() {
                        if (index < values.length) return Promise.resolve({ value: values[index++], done: false });
                        return Promise.resolve({ done: true });
                    },
                    return() {
                        returnCalled = true;
                        return Promise.resolve({ done: true });
                    },
                };
            },
            get returnCalled() {
                return returnCalled;
            },
        };
    }

    test("break calls return()", () => {
        const iter = makeAsyncIterator([1, 2, 3]);
        return (async () => {
            for await (const x of iter) {
                break;
            }
        })().then(() => {
            expect(iter.returnCalled).toBeTrue();
        });
    });

    test("return from async function calls return()", () => {
        const iter = makeAsyncIterator([1, 2, 3]);
        return (async () => {
            for await (const x of iter) {
                return x;
            }
        })().then(() => {
            expect(iter.returnCalled).toBeTrue();
        });
    });

    test("throw from body calls return()", () => {
        const iter = makeAsyncIterator([1, 2, 3]);
        return (async () => {
            for await (const x of iter) {
                throw new Error("oops");
            }
        })().catch(() => {
            expect(iter.returnCalled).toBeTrue();
        });
    });

    test("continue to same loop does NOT call return()", () => {
        const iter = makeAsyncIterator([1, 2, 3]);
        return (async () => {
            for await (const x of iter) {
                continue;
            }
        })().then(() => {
            expect(iter.returnCalled).toBeFalse();
        });
    });

    test("normal completion does NOT call return()", () => {
        const iter = makeAsyncIterator([1, 2, 3]);
        return (async () => {
            for await (const x of iter) {
                // exhaust normally
            }
        })().then(() => {
            expect(iter.returnCalled).toBeFalse();
        });
    });

    test("continue to outer label calls return()", () => {
        const iter = makeAsyncIterator([1, 2, 3]);
        return (async () => {
            outer: for (let i = 0; i < 1; i++) {
                for await (const x of iter) {
                    continue outer;
                }
            }
        })().then(() => {
            expect(iter.returnCalled).toBeTrue();
        });
    });

    test("return() that throws during non-throw exit: close error propagates", () => {
        const iter = {
            [Symbol.asyncIterator]() {
                return {
                    next() {
                        return Promise.resolve({ value: 1, done: false });
                    },
                    return() {
                        return Promise.reject(new Error("close error"));
                    },
                };
            },
        };
        return (async () => {
            for await (const x of iter) {
                break;
            }
        })().then(
            () => {
                expect().fail("should have thrown");
            },
            error => {
                expect(error).toBeInstanceOf(Error);
                expect(error.message).toBe("close error");
            }
        );
    });

    test("return() that throws during throw exit: original throw takes precedence", () => {
        const iter = {
            [Symbol.asyncIterator]() {
                return {
                    next() {
                        return Promise.resolve({ value: 1, done: false });
                    },
                    return() {
                        return Promise.reject(new Error("close error"));
                    },
                };
            },
        };
        return (async () => {
            for await (const x of iter) {
                throw new Error("body error");
            }
        })().then(
            () => {
                expect().fail("should have thrown");
            },
            error => {
                expect(error).toBeInstanceOf(Error);
                expect(error.message).toBe("body error");
            }
        );
    });

    test("iterator without return() method: no error on break", () => {
        const iter = {
            [Symbol.asyncIterator]() {
                let index = 0;
                return {
                    next() {
                        return Promise.resolve(index++ < 3 ? { value: index, done: false } : { done: true });
                    },
                };
            },
        };
        return (async () => {
            for await (const x of iter) {
                break;
            }
        })();
    });

    test("GetMethod throwing during throw exit: original throw takes precedence", () => {
        const iter = {
            [Symbol.asyncIterator]() {
                return {
                    next() {
                        return Promise.resolve({ value: 1, done: false });
                    },
                    get return() {
                        throw { name: "inner error" };
                    },
                };
            },
        };
        return (async () => {
            for await (const x of iter) {
                throw new Error("original");
            }
        })().then(
            () => {
                expect().fail("should have thrown");
            },
            error => {
                expect(error).toBeInstanceOf(Error);
                expect(error.message).toBe("original");
            }
        );
    });

    test("non-callable return during throw exit: original throw takes precedence", () => {
        const iter = {
            [Symbol.asyncIterator]() {
                return {
                    next() {
                        return Promise.resolve({ value: 1, done: false });
                    },
                    return: true,
                };
            },
        };
        return (async () => {
            for await (const x of iter) {
                throw new Error("original");
            }
        })().then(
            () => {
                expect().fail("should have thrown");
            },
            error => {
                expect(error).toBeInstanceOf(Error);
                expect(error.message).toBe("original");
            }
        );
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

        return (async () => {
            for await (const x of syncIter) {
                // The rejected promise causes the async-from-sync iterator
                // to call return() internally. Our for-await-of close logic
                // must not double-close.
            }
        })().catch(() => {
            expect(returnCount).toBe(1);
        });
    });
});
