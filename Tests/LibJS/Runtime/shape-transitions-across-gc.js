test("objects built by a constructor keep their shape across garbage collection", () => {
    function Point(x, y) {
        this.x = x;
        this.y = y;
        this.z = x + y;
    }
    const kept = new Point(1, 2);
    // Only `kept` uses the Point shapes; the shapes it went through while being built are referenced by nothing else.
    for (let i = 0; i < 1000; ++i) new Point(3, 4);
    gc();
    gc();
    expect(haveSameShape(new Point(3, 4), kept)).toBeTrue();
});

test("objects built by a literal and additions keep their shape across garbage collection", () => {
    const build = () => {
        const object = {};
        object.first = 1;
        object.second = 2;
        delete object.second;
        object.third = 3;
        return object;
    };
    const kept = build();
    gc();
    gc();
    expect(haveSameShape(build(), kept)).toBeTrue();
});

test("transition chains that no object uses are collected", () => {
    evaluateSource(`
        var __shapeChainPrototype = {};
        var __shapeChainObject = Object.create(__shapeChainPrototype);
        __shapeChainObject.a = 1;
        __shapeChainObject.b = 2;
    `);
    const registry = new FinalizationRegistry(() => {});
    registry.register(globalThis.__shapeChainPrototype, "prototype");
    markAsGarbage("__shapeChainPrototype");
    markAsGarbage("__shapeChainObject");
    gc();

    // The shapes of the object keep its prototype alive, so the prototype dies only if its shapes do.
    let collected = false;
    cleanupFinalizationRegistry(registry, () => {
        collected = true;
    });
    expect(collected).toBeTrue();
});

test("haveSameShape() needs objects", () => {
    expect(() => haveSameShape({}, 1)).toThrowWithMessage(TypeError, "1 is not an object");
    expect(haveSameShape({ a: 1 }, { a: 2 })).toBeTrue();
    expect(haveSameShape({ a: 1 }, { b: 2 })).toBeFalse();
});

test("an object that changes its prototype does not keep its old prototypes alive", () => {
    evaluateSource(`
        var __changingPrototypeObject = {};
        var __replacedPrototype = {};
        Object.setPrototypeOf(__changingPrototypeObject, __replacedPrototype);
        Object.setPrototypeOf(__changingPrototypeObject, {});
    `);
    const registry = new FinalizationRegistry(() => {});
    registry.register(globalThis.__replacedPrototype, "replaced prototype");
    markAsGarbage("__replacedPrototype");
    gc();

    let collected = false;
    cleanupFinalizationRegistry(registry, () => {
        collected = true;
    });
    expect(collected).toBeTrue();
});
