for (const cleanupThrows of [false, true]) {
    test(`async yield* preserves state across return and missing throw when cleanup throws: ${cleanupThrows}`, () => {
        const kept = { tag: "kept" };
        const cleanupError = {};
        const events = [];
        let caught;
        const delegate = {
            [Symbol.asyncIterator]() {
                return this;
            },
            next() {
                return { value: "first", done: false };
            },
            async return() {
                events.push(`return:${arguments.length}`);
                if (arguments.length !== 0) {
                    return { value: "return yield", done: false };
                }
                await Promise.resolve();
                events.push("cleanup finished");
                if (cleanupThrows) {
                    throw cleanupError;
                }
                return { done: false };
            },
        };
        async function* outer() {
            try {
                return [{ kept }, yield* delegate];
            } catch (error) {
                caught = error;
                yield kept;
                return "recovered";
            } finally {
                events.push(kept.tag);
            }
        }

        let results;
        let rejection;
        (async () => {
            const iterator = outer();
            return [
                await iterator.next(),
                await iterator.return("continue"),
                await iterator.throw("original"),
                await iterator.next(),
            ];
        })().then(
            value => (results = value),
            error => (rejection = error)
        );
        runQueuedPromiseJobs();

        expect(rejection).toBeUndefined();
        expect(results).toEqual([
            { value: "first", done: false },
            { value: "return yield", done: false },
            { value: kept, done: false },
            { value: "recovered", done: true },
        ]);
        expect(events).toEqual(["return:1", "return:0", "cleanup finished", "kept"]);
        if (cleanupThrows) {
            expect(caught).toBe(cleanupError);
        } else {
            expect(caught).toBeInstanceOf(TypeError);
        }
    });
}

test("async yield* preserves an earlier expression value through delegation", () => {
    const kept = {};
    async function* delegate() {
        yield 1;
        return 2;
    }
    async function* outer() {
        return [{ kept }, yield* delegate()];
    }

    let results;
    let rejection;
    (async () => {
        const iterator = outer();
        return [await iterator.next(), await iterator.next()];
    })().then(
        value => (results = value),
        error => (rejection = error)
    );
    runQueuedPromiseJobs();

    expect(rejection).toBeUndefined();
    expect(results).toEqual([
        { value: 1, done: false },
        { value: [{ kept }, 2], done: true },
    ]);
    expect(results[1].value[0].kept).toBe(kept);
});
