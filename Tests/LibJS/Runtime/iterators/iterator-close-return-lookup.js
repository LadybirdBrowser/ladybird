const ArrayIteratorPrototype = Object.getPrototypeOf([][Symbol.iterator]());
const IteratorPrototype = Object.getPrototypeOf(ArrayIteratorPrototype);

function destructureTwo(iterable) {
    const [first, second] = iterable;
    return [first, second];
}

function breakAfterFirst(iterable) {
    for (const value of iterable) return value;
}

function closeArrayIterators() {
    for (let i = 0; i < 100; ++i) {
        expect(destructureTwo([1, 2, 3])).toEqual([1, 2]);
        expect(breakAfterFirst([1, 2, 3])).toBe(1);
    }
}

function expectReturnMethodOn(object) {
    closeArrayIterators();
    const receivers = [];
    object.return = function () {
        receivers.push(this);
        return {};
    };
    try {
        expect(destructureTwo([1, 2, 3])).toEqual([1, 2]);
        expect(breakAfterFirst([1, 2, 3])).toBe(1);
        expect(receivers).toHaveLength(2);
        for (const receiver of receivers) expect(Object.getPrototypeOf(receiver)).toBe(ArrayIteratorPrototype);
    } finally {
        delete object.return;
    }
    closeArrayIterators();
    expect(receivers).toHaveLength(2);
}

describe("closing an iterator looks up return every time", () => {
    test("return added to %ArrayIteratorPrototype%", () => {
        expectReturnMethodOn(ArrayIteratorPrototype);
    });

    test("return added to %IteratorPrototype%", () => {
        expectReturnMethodOn(IteratorPrototype);
    });

    test("return added to Object.prototype", () => {
        expectReturnMethodOn(Object.prototype);
    });

    test("return added to a prototype with a dictionary shape", () => {
        for (let i = 0; i < 100; ++i) IteratorPrototype[`property${i}`] = i;
        for (let i = 0; i < 100; ++i) delete IteratorPrototype[`property${i}`];
        expectReturnMethodOn(IteratorPrototype);
    });

    test("a getter for return runs on every close", () => {
        closeArrayIterators();
        let getterCalls = 0;
        Object.defineProperty(IteratorPrototype, "return", {
            get() {
                ++getterCalls;
                return undefined;
            },
            configurable: true,
        });
        try {
            for (let i = 0; i < 10; ++i) {
                destructureTwo([1, 2, 3]);
                breakAfterFirst([1, 2, 3]);
            }
            expect(getterCalls).toBe(20);
        } finally {
            delete IteratorPrototype.return;
        }
    });

    test("an own return method", () => {
        let returnCalls = 0;
        function makeIterable() {
            return {
                [Symbol.iterator]() {
                    return {
                        next() {
                            return { value: 1, done: false };
                        },
                        return() {
                            ++returnCalls;
                            return {};
                        },
                    };
                },
            };
        }
        for (let i = 0; i < 100; ++i) {
            destructureTwo(makeIterable());
            breakAfterFirst(makeIterable());
        }
        expect(returnCalls).toBe(200);
    });

    test("generators run their finally blocks", () => {
        let finallyRuns = 0;
        function* generator() {
            try {
                yield 1;
                yield 2;
                yield 3;
            } finally {
                ++finallyRuns;
            }
        }
        for (let i = 0; i < 100; ++i) {
            expect(destructureTwo(generator())).toEqual([1, 2]);
            expect(breakAfterFirst(generator())).toBe(1);
        }
        expect(finallyRuns).toBe(200);
    });

    test("proxies see every lookup of return", () => {
        let returnLookups = 0;
        function makeIterable() {
            const iterator = new Proxy(
                {
                    next() {
                        return { value: 1, done: false };
                    },
                },
                {
                    get(target, key, receiver) {
                        if (key === "return") ++returnLookups;
                        return Reflect.get(target, key, receiver);
                    },
                }
            );
            return { [Symbol.iterator]: () => iterator };
        }
        for (let i = 0; i < 100; ++i) {
            destructureTwo(makeIterable());
            breakAfterFirst(makeIterable());
        }
        expect(returnLookups).toBe(200);
    });
});

describe("yield* looks up return and throw every time", () => {
    class Inner {
        [Symbol.iterator]() {
            return this;
        }

        next() {
            return { value: 1, done: false };
        }
    }

    function* outer(inner) {
        return yield* inner;
    }

    test("return added to the prototype of the inner iterator", () => {
        for (let i = 0; i < 100; ++i) {
            const generator = outer(new Inner());
            generator.next();
            expect(generator.return(5)).toEqual({ value: 5, done: true });
        }
        let returnCalls = 0;
        Inner.prototype.return = function (value) {
            ++returnCalls;
            return { value: value * 2, done: true };
        };
        try {
            const generator = outer(new Inner());
            generator.next();
            expect(generator.return(5)).toEqual({ value: 10, done: true });
            expect(returnCalls).toBe(1);
        } finally {
            delete Inner.prototype.return;
        }
    });

    test("throw added to the prototype of the inner iterator", () => {
        for (let i = 0; i < 100; ++i) {
            const generator = outer(new Inner());
            generator.next();
            expect(() => generator.throw(new Error("thrown"))).toThrow(TypeError);
        }
        let throwCalls = 0;
        Inner.prototype.throw = function () {
            ++throwCalls;
            return { value: "caught", done: true };
        };
        try {
            const generator = outer(new Inner());
            generator.next();
            expect(generator.throw(new Error("thrown"))).toEqual({ value: "caught", done: true });
            expect(throwCalls).toBe(1);
        } finally {
            delete Inner.prototype.throw;
        }
    });
});
