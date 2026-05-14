test("cleanupSome is not exposed", () => {
    expect(FinalizationRegistry.prototype.cleanupSome).toBeUndefined();
});

function registerInDifferentScope(registry) {
    const target = {};
    registry.register(target, {});
    return target;
}

test.xfail("basic functionality", () => {
    var registry = new FinalizationRegistry(() => {});

    var count = 0;
    var increment = () => {
        count++;
    };

    cleanupFinalizationRegistry(registry, increment);

    expect(count).toBe(0);

    const target = registerInDifferentScope(registry);
    markAsGarbage("target");
    gc();

    cleanupFinalizationRegistry(registry, increment);

    expect(count).toBe(1);
});

test("callback can unregister the next record after the current record dies", () => {
    var token2 = {};
    var heldValues = [];
    var registry = new FinalizationRegistry(held => {
        heldValues.push(held);
        if (held === "first") expect(registry.unregister(token2)).toBeTrue();
    });

    evaluateSource("var __finalizationRegistryFirst = {};");
    evaluateSource("var __finalizationRegistrySecond = {};");
    registry.register(globalThis.__finalizationRegistryFirst, "first");
    registry.register(globalThis.__finalizationRegistrySecond, "second", token2);
    markAsGarbage("__finalizationRegistryFirst");
    markAsGarbage("__finalizationRegistrySecond");
    gc();

    cleanupFinalizationRegistry(registry);

    expect(heldValues).toEqual(["first"]);
    expect(registry.unregister(token2)).toBeFalse();
});

test("cleanup can process multiple dead records from one garbage collection", () => {
    var heldValues = [];
    var registry = new FinalizationRegistry(value => {
        heldValues.push(value);
    });

    evaluateSource("var __finalizationRegistryCleanupTarget1 = {};");
    evaluateSource("var __finalizationRegistryCleanupTarget2 = {};");
    registry.register(globalThis.__finalizationRegistryCleanupTarget1, "first");
    registry.register(globalThis.__finalizationRegistryCleanupTarget2, "second");
    markAsGarbage("__finalizationRegistryCleanupTarget1");
    markAsGarbage("__finalizationRegistryCleanupTarget2");
    gc();

    cleanupFinalizationRegistry(registry);

    expect(heldValues).toEqual(["first", "second"]);
});

test("cleanup helper errors", () => {
    var registry = new FinalizationRegistry(() => {});

    expect(() => {
        cleanupFinalizationRegistry(registry, 5);
    }).toThrowWithMessage(TypeError, "is not a function");

    expect(() => {
        cleanupFinalizationRegistry({});
    }).toThrowWithMessage(TypeError, "Not an object of type FinalizationRegistry");
});
