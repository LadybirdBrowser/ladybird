// Objects made with a prototype get their shape through the VM's cache of
// recent prototype transitions, in front of the transition tables of shapes.

test("objects made with the same prototype share a shape", () => {
    const prototype = { greet() {} };
    expect(haveSameShape(Object.create(prototype), Object.create(prototype))).toBeTrue();
    expect(haveSameShape(Object.create(prototype), Object.create({}))).toBeFalse();
    class Point {}
    expect(haveSameShape(new Point(), new Point())).toBeTrue();
    expect(haveSameShape(/a/.exec("a"), /b/.exec("b"))).toBeTrue();
});

test("many prototypes", () => {
    const prototypes = [];
    for (let i = 0; i < 2000; ++i) prototypes.push({ index: i });
    for (let round = 0; round < 3; ++round) {
        for (let i = 0; i < prototypes.length; ++i) {
            const object = Object.create(prototypes[i]);
            expect(Object.getPrototypeOf(object)).toBe(prototypes[i]);
            expect(object.index).toBe(i);
        }
    }
});

test("shapes stay shared across garbage collection", () => {
    const prototype = {};
    const kept = Object.create(prototype);
    gc();
    expect(haveSameShape(Object.create(prototype), kept)).toBeTrue();
});

test("objects made with a prototype do not keep it alive once they are gone", () => {
    evaluateSource(`
        var __cachedTransitionPrototype = {};
        for (let i = 0; i < 10; ++i) Object.create(__cachedTransitionPrototype);
    `);
    const registry = new FinalizationRegistry(() => {});
    registry.register(globalThis.__cachedTransitionPrototype, "prototype");
    markAsGarbage("__cachedTransitionPrototype");
    gc();

    let collected = false;
    cleanupFinalizationRegistry(registry, () => {
        collected = true;
    });
    expect(collected).toBeTrue();

    // Prototypes made after the collection, which may reuse its cells, get shapes of their own.
    for (let i = 0; i < 100; ++i) {
        const prototype = { index: i };
        expect(Object.getPrototypeOf(Object.create(prototype))).toBe(prototype);
    }
});
