const IteratorPrototype = Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()));

let returnCalls = 0;
IteratorPrototype.return = function () {
    ++returnCalls;
    return {};
};

test("a builtin step that throws marks the iterator record as done", () => {
    for (const combinator of ["all", "allSettled", "any", "race"]) {
        returnCalls = 0;
        const typedArray = new Uint8Array(4);

        class DetachingPromise extends Promise {
            static resolve(value) {
                detachArrayBuffer(typedArray.buffer);
                return super.resolve(value);
            }
        }

        let rejection;
        DetachingPromise[combinator](typedArray.values()).catch(error => {
            rejection = error;
        });
        runQueuedPromiseJobs();

        expect(rejection).toBeInstanceOf(TypeError);
        expect(returnCalls).toBe(0);
    }
});
