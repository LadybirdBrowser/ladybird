const GeneratorPrototype = Object.getPrototypeOf(function* () {}).prototype;

function* threeValues() {
    yield 1;
    yield 2;
    yield 3;
}

function withRecordingGeneratorReturn(callback) {
    const originalReturn = GeneratorPrototype.return;
    let returnCalls = 0;
    GeneratorPrototype.return = function () {
        ++returnCalls;
        return originalReturn.apply(this, arguments);
    };
    try {
        callback();
    } finally {
        GeneratorPrototype.return = originalReturn;
    }
    return returnCalls;
}

// An iterator for yield* whose results log every read of done and value. The fourth result is done.
function makeLoggingIterator(log, throwingGetter) {
    let count = 0;
    return {
        [Symbol.iterator]() {
            return this;
        },
        next() {
            const n = ++count;
            let doneReads = 0;
            return {
                get done() {
                    log.push(`done ${n}`);
                    // NB: yield* reads done first, so the second read is the one of the consumer.
                    if (throwingGetter?.done === n && ++doneReads === 2) throw new Error("done getter");
                    return n > 3;
                },
                get value() {
                    log.push(`value ${n}`);
                    if (throwingGetter?.value === n) throw new Error("value getter");
                    return n;
                },
            };
        },
        return() {
            log.push("return");
            return {};
        },
    };
}

function* delegate(iterator) {
    return yield* iterator;
}

test("an own next method of a generator is used", () => {
    const generator = threeValues();
    let nextCalls = 0;
    generator.next = function () {
        ++nextCalls;
        return GeneratorPrototype.next.call(this);
    };
    expect([...generator]).toEqual([1, 2, 3]);
    expect(nextCalls).toBe(4);
});

test("iterating a generator", () => {
    const values = [];
    for (const value of threeValues()) values.push(value);
    expect(values).toEqual([1, 2, 3]);
    expect([...threeValues()]).toEqual([1, 2, 3]);
    expect(Array.from(threeValues())).toEqual([1, 2, 3]);
    const [first, second, ...rest] = threeValues();
    expect([first, second, rest]).toEqual([1, 2, [3]]);
    expect(
        threeValues()
            .map(value => value * 2)
            .toArray()
    ).toEqual([2, 4, 6]);
    expect(threeValues().drop(1).toArray()).toEqual([2, 3]);
    expect(threeValues().take(2).toArray()).toEqual([1, 2]);
});

test("array destructuring closes a generator it does not exhaust", () => {
    const log = [];
    function* generator() {
        try {
            yield 1;
            yield 2;
        } finally {
            log.push("finally");
        }
    }
    const [first] = generator();
    expect(first).toBe(1);
    expect(log).toEqual(["finally"]);
});

test("the return value of a generator is not iterated", () => {
    function* generator() {
        yield 1;
        return 2;
    }
    const values = [];
    for (const value of generator()) values.push(value);
    expect(values).toEqual([1]);
    expect([...generator()]).toEqual([1]);
    expect(Array.from(generator())).toEqual([1]);
});

test("a generator that throws is not closed", () => {
    function* generator() {
        yield 1;
        throw new Error("generator body");
    }
    let rejection;
    const returnCalls = withRecordingGeneratorReturn(() => {
        expect(() => {
            for (const value of generator());
        }).toThrowWithMessage(Error, "generator body");
        Promise.all(generator()).catch(error => {
            rejection = error;
        });
    });
    runQueuedPromiseJobs();
    expect(rejection.message).toBe("generator body");
    expect(returnCalls).toBe(0);
});

test("stepping a generator that is executing throws", () => {
    let generator;
    function* callsNext() {
        yield 1;
        generator.next();
    }
    generator = callsNext();
    expect(() => {
        for (const value of generator);
    }).toThrowWithMessage(TypeError, "Generator is already executing");

    function* iteratesItself() {
        yield 1;
        for (const value of generator);
    }
    generator = iteratesItself();
    expect(() => {
        for (const value of generator);
    }).toThrowWithMessage(TypeError, "Generator is already executing");
});

test("yield* results are read as before", () => {
    const expectedLog = [
        "done 1",
        "done 1",
        "value 1",
        "done 2",
        "done 2",
        "value 2",
        "done 3",
        "done 3",
        "value 3",
        "done 4",
        "value 4",
    ];

    let log = [];
    const values = [];
    for (const value of delegate(makeLoggingIterator(log))) values.push(value);
    expect(values).toEqual([1, 2, 3]);
    expect(log).toEqual(expectedLog);

    log = [];
    expect([...delegate(makeLoggingIterator(log))]).toEqual([1, 2, 3]);
    expect(log).toEqual(expectedLog);

    log = [];
    const [first] = delegate(makeLoggingIterator(log));
    expect(first).toBe(1);
    expect(log).toEqual(["done 1", "done 1", "value 1", "return"]);
});

test("yield* results skipped by IteratorStep do not have their value read", () => {
    const log = [];
    expect(delegate(makeLoggingIterator(log)).drop(2).toArray()).toEqual([3]);
    expect(log).toEqual(["done 1", "done 1", "done 2", "done 2", "done 3", "done 3", "value 3", "done 4", "value 4"]);
});

test("a yield* result with a throwing getter marks the iteration as done", () => {
    for (const getter of ["done", "value"]) {
        const log = [];
        let rejection;
        const returnCalls = withRecordingGeneratorReturn(() => {
            Promise.all(delegate(makeLoggingIterator(log, { [getter]: 2 }))).catch(error => {
                rejection = error;
            });
        });
        runQueuedPromiseJobs();
        expect(rejection.message).toBe(`${getter} getter`);
        expect(returnCalls).toBe(0);
        expect(log.includes("return")).toBeFalse();
    }
});

test("%GeneratorPrototype%.next rejects iterator helpers", () => {
    const helper = [1, 2].values().map(value => value * 2);
    expect(() => GeneratorPrototype.next.call(helper)).toThrowWithMessage(
        TypeError,
        "Generator brand 'Iterator Helper' does not match generator brand '<empty>'"
    );
    expect([...helper]).toEqual([2, 4]);

    const helperWithGeneratorNext = [1, 2].values().map(value => value * 2);
    helperWithGeneratorNext.next = GeneratorPrototype.next;
    expect(() => {
        for (const value of helperWithGeneratorNext);
    }).toThrowWithMessage(TypeError, "Generator brand 'Iterator Helper' does not match generator brand '<empty>'");
});
