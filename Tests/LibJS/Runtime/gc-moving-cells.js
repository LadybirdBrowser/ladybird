// The objects below are only reachable through the properties of `holder`, which the collector can rewrite, so a
// collection that moves cells moves them.
const holder = {
    small: { a: 1, b: "two" },
    large: {},
    array: [1, 2, 3, { x: 4 }],
    hashed: {},
    mapKey: {},
    weakMapKey: {},
    counter: (() => {
        let count = 0;
        return () => ++count;
    })(),
};
holder.counter();
// An environment with this many bindings keeps their values outside its cell.
const wideBindingCount = 40;
holder.wide = new Function(
    `let ${Array.from({ length: wideBindingCount }, (_, i) => `b${i} = { v: ${i} }`).join(", ")};` +
        `return () => [${Array.from({ length: wideBindingCount }, (_, i) => `b${i}`).join(", ")}];`
)();
holder.rope = "x".repeat(30) + "y".repeat(30);
holder.substring = holder.rope.substring(25, 35);
holder.flat = String(holder.rope.length) + "!";
for (let i = 0; i < 40; ++i) holder.large["property" + i] = i;
holder.large.nested = { y: 5 };

const before = {
    small: addressOf(holder.small),
    large: addressOf(holder.large),
    array: addressOf(holder.array),
    hashed: addressOf(holder.hashed),
    hash: identityHashOf(holder.hashed),
    counter: addressOf(holder.counter),
    wideBinding: addressOf(holder.wide()[wideBindingCount - 1]),
    rope: addressOf(holder.rope),
};

const map = new Map([[holder.mapKey, "map value"]]);

// The entries of maps and sets, the elements of arrays too large for a storage cell, the values of environments with
// more bindings than fit in one, and the target and arguments of bound functions are all slots the collector can
// rewrite, so their cells move too.
const entryCount = 40;
const bigMap = new Map();
const bigSet = new Set();
for (let i = 0; i < entryCount; ++i) {
    bigMap.set({ k: i }, { v: i });
    bigSet.add({ s: i });
}
const smallSet = new Set([{ s: "only" }]);
const hugeArray = Array.from({ length: 2000 }, (_, i) => ({ e: i }));
const hugeBindingCount = 1500;
const huge = new Function(
    `let ${Array.from({ length: hugeBindingCount }, (_, i) => `h${i} = { v: ${i} }`).join(", ")};` +
        `return () => h${hugeBindingCount - 1};`
)();
const bound = function (a, b) {
    return [this.t, a.x, b];
}.bind({ t: "this" }, { x: "argument" });
const movedBefore = {
    mapKey: addressOf(bigMap.keys().next().value),
    mapValue: addressOf(bigMap.values().next().value),
    setValue: addressOf(bigSet.values().next().value),
    smallSetValue: addressOf(smallSet.values().next().value),
    hugeElement: addressOf(hugeArray[1999]),
    hugeBinding: addressOf(huge()),
};
const weakMap = new WeakMap([[holder.weakMapKey, "weak map value"]]);

// Weak collections hand the slots of the cells that survive to the collector once they forgot the dead ones, so their
// keys, values and targets move too.
const weakHolder = {
    key: {},
    primitiveKey: {},
    setValue: {},
    refTarget: {},
    registryTarget: {},
    registryToken: {},
};
const weakValueMap = new WeakMap([
    [weakHolder.key, { value: "weak value" }],
    [weakHolder.primitiveKey, 42],
]);
const weakSet = new WeakSet([weakHolder.setValue]);
const weakRef = new WeakRef(weakHolder.refTarget);
const registry = new FinalizationRegistry(() => {});
registry.register(weakHolder.registryTarget, "held", weakHolder.registryToken);
const weakBefore = {
    key: addressOf(weakHolder.key),
    primitiveKey: addressOf(weakHolder.primitiveKey),
    value: addressOf(weakValueMap.get(weakHolder.key)),
    setValue: addressOf(weakHolder.setValue),
    refTarget: addressOf(weakHolder.refTarget),
    registryTarget: addressOf(weakHolder.registryTarget),
};

// Constants of executables are slots the collector can rewrite. Strings this long are not interned.
const constantString = () =>
    "a constant string that is long enough not to be interned, which only this function holds ..............................................................................................................................................................................................................................";
const constantBigInt = () => 123456789012345678901234567890n;
const internedConstant = () => "an interned constant";
// NB: JIT code that embeds a constant keeps its cell where it is.
jit.neverCompile(constantString);
jit.neverCompile(constantBigInt);
jit.neverCompile(internedConstant);
const constantsBefore = {
    string: addressOf(constantString()),
    bigint: addressOf(constantBigInt()),
    interned: addressOf(internedConstant()),
};

// The key strings of for-in caches and of the own keys of shapes are slots the collector can rewrite.
const keyed = { firstKey: 1, secondKey: 2 };
const forInKeys = object => {
    const keys = [];
    for (const key in object) keys.push(key);
    return keys;
};
forInKeys(keyed);
Object.keys(keyed);

// JIT code that creates closures copies a sample, which the code keeps in a slot the collector can rewrite, so the
// sample can move.
function makeAdder(n) {
    return x => x + n;
}
jit.prepare(makeAdder);
for (let i = 0; i < 10; ++i) makeAdder(i)(i);
jit.compile(makeAdder);

runAfterMovingCollection(() => {
    test("objects with inline storage keep their properties when they move", () => {
        expect(addressOf(holder.small)).not.toBe(before.small);
        expect(holder.small.a).toBe(1);
        expect(holder.small.b).toBe("two");
        holder.small.c = 3;
        expect(Object.keys(holder.small)).toEqual(["a", "b", "c"]);
    });

    test("objects with out-of-line storage keep their properties when they move", () => {
        expect(addressOf(holder.large)).not.toBe(before.large);
        for (let i = 0; i < 40; ++i) expect(holder.large["property" + i]).toBe(i);
        expect(holder.large.nested.y).toBe(5);
    });

    test("arrays keep their elements when they move", () => {
        expect(addressOf(holder.array)).not.toBe(before.array);
        expect(holder.array.length).toBe(4);
        expect(holder.array[3].x).toBe(4);
        holder.array.push(5);
        expect(holder.array).toEqual([1, 2, 3, { x: 4 }, 5]);
    });

    test("identity hashes stay the same when cells move", () => {
        expect(addressOf(holder.hashed)).not.toBe(before.hashed);
        expect(identityHashOf(holder.hashed)).toBe(before.hash);
        const set = new Set([holder.hashed]);
        expect(set.has(holder.hashed)).toBeTrue();
    });

    test("closures keep their environments when they move", () => {
        expect(addressOf(holder.counter)).not.toBe(before.counter);
        expect(holder.counter()).toBe(2);
        expect(holder.counter()).toBe(3);
    });

    test("environments with many bindings keep their values when the values move", () => {
        const values = holder.wide();
        expect(addressOf(values[wideBindingCount - 1])).not.toBe(before.wideBinding);
        expect(values.map(value => value.v)).toEqual(Array.from({ length: wideBindingCount }, (_, i) => i));
    });

    test("strings keep their contents when they move", () => {
        expect(addressOf(holder.rope)).not.toBe(before.rope);
        expect(holder.rope).toBe("x".repeat(30) + "y".repeat(30));
        expect(holder.substring).toBe("xxxxxyyyyy");
        expect(holder.flat).toBe("60!");
    });

    test("maps and weak maps find their keys after the collection", () => {
        expect(map.get(holder.mapKey)).toBe("map value");
        expect(weakMap.get(holder.weakMapKey)).toBe("weak map value");
        expect(map.has({})).toBeFalse();
    });

    test("map and set entries move, and keep their order and lookups", () => {
        const keys = [...bigMap.keys()];
        expect(addressOf(keys[0])).not.toBe(movedBefore.mapKey);
        expect(addressOf(bigMap.get(keys[0]))).not.toBe(movedBefore.mapValue);
        expect(keys.map(key => key.k)).toEqual(Array.from({ length: entryCount }, (_, i) => i));
        for (const key of keys) expect(bigMap.get(key).v).toBe(key.k);
        const values = [...bigSet];
        expect(addressOf(values[0])).not.toBe(movedBefore.setValue);
        for (const value of values) expect(bigSet.has(value)).toBeTrue();
        expect(values.map(value => value.s)).toEqual(Array.from({ length: entryCount }, (_, i) => i));
        bigMap.delete(keys[0]);
        expect(bigMap.has(keys[0])).toBeFalse();
        expect(bigMap.size).toBe(entryCount - 1);
        const [only] = smallSet;
        expect(addressOf(only)).not.toBe(movedBefore.smallSetValue);
        expect(smallSet.has(only)).toBeTrue();
    });

    test("elements and bindings in storage too large for a cell move", () => {
        expect(addressOf(hugeArray[1999])).not.toBe(movedBefore.hugeElement);
        expect(hugeArray[1999].e).toBe(1999);
        expect(addressOf(huge())).not.toBe(movedBefore.hugeBinding);
        expect(huge().v).toBe(hugeBindingCount - 1);
    });

    test("bound functions keep their target, this and arguments", () => {
        expect(bound(2)).toEqual(["this", "argument", 2]);
    });

    test("weak map keys and values move, and keep their lookups", () => {
        expect(addressOf(weakHolder.key)).not.toBe(weakBefore.key);
        expect(addressOf(weakHolder.primitiveKey)).not.toBe(weakBefore.primitiveKey);
        expect(addressOf(weakValueMap.get(weakHolder.key))).not.toBe(weakBefore.value);
        expect(weakValueMap.get(weakHolder.key).value).toBe("weak value");
        expect(weakValueMap.get(weakHolder.primitiveKey)).toBe(42);
        expect(weakValueMap.has({})).toBeFalse();
        expect(weakValueMap.delete(weakHolder.primitiveKey)).toBeTrue();
        expect(weakValueMap.has(weakHolder.primitiveKey)).toBeFalse();
    });

    test("weak set values, weak ref targets and finalization registry targets move", () => {
        expect(addressOf(weakHolder.setValue)).not.toBe(weakBefore.setValue);
        expect(weakSet.has(weakHolder.setValue)).toBeTrue();
        expect(addressOf(weakHolder.refTarget)).not.toBe(weakBefore.refTarget);
        expect(weakRef.deref()).toBe(weakHolder.refTarget);
        expect(addressOf(weakHolder.registryTarget)).not.toBe(weakBefore.registryTarget);
        expect(registry.unregister(weakHolder.registryToken)).toBeTrue();
    });

    test("constants of executables move", () => {
        expect(addressOf(constantString())).not.toBe(constantsBefore.string);
        expect(constantString().startsWith("a constant string")).toBeTrue();
        expect(constantString().length).toBeGreaterThan(256);
        expect(addressOf(constantBigInt())).not.toBe(constantsBefore.bigint);
        expect(constantBigInt()).toBe(123456789012345678901234567890n);
    });

    test("interned strings move, and stay the interned strings of their contents", () => {
        expect(addressOf(internedConstant())).not.toBe(constantsBefore.interned);
        expect(internedConstant()).toBe("an interned constant");
        const object = { "an interned constant": 1 };
        expect(object[internedConstant()]).toBe(1);
    });

    test("cached key strings keep their contents when they move", () => {
        expect(forInKeys(keyed)).toEqual(["firstKey", "secondKey"]);
        expect(Object.keys(keyed)).toEqual(["firstKey", "secondKey"]);
    });

    test("compiled code creates closures after their sample moved", () => {
        const adders = Array.from({ length: 10 }, (_, i) => makeAdder(i));
        expect(adders.map((adder, i) => adder(i))).toEqual(Array.from({ length: 10 }, (_, i) => 2 * i));
    });
});

runAfterMovingCollection(() => {
    const addressAfterFirstMove = addressOf(holder.small);
    runAfterMovingCollection(() => {
        test("callbacks that a callback asks for run after another collection that moves cells", () => {
            expect(addressOf(holder.small)).not.toBe(addressAfterFirstMove);
            expect(holder.small.c).toBe(3);
        });
    });
});
