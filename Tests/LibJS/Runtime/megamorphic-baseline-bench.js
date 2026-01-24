// Megamorphic Inline Cache Baseline Performance Test
//
// This test establishes baseline performance metrics for property access
// before implementing the megamorphic inline cache optimization.
//
// Tests include:
// 1. Monomorphic property access (single shape)
// 2. Polymorphic property access (2-4 shapes)
// 3. Megamorphic property access (many shapes)

function benchmark(name, iterations, fn) {
    // Warmup
    for (let i = 0; i < 1000; i++) fn();
    
    gc();
    
    const start = Date.now();
    for (let i = 0; i < iterations; i++) {
        fn();
    }
    const elapsed = Date.now() - start;
    
    console.log(`${name}: ${elapsed}ms (${iterations} iterations, ${(elapsed / iterations * 1000).toFixed(3)} µs/iter)`);
    return elapsed;
}

// ====== GET PROPERTY BENCHMARKS ======

console.log("=== GET PROPERTY BENCHMARKS ===\n");

// Monomorphic get: single shape, should be fastest
benchmark("get-monomorphic", 1000000, () => {
    const obj = { x: 1, y: 2, z: 3 };
    let sum = 0;
    for (let i = 0; i < 100; i++) {
        sum += obj.x;
    }
    return sum;
});

// Polymorphic get: 2 shapes
benchmark("get-polymorphic-2", 1000000, () => {
    const obj1 = { x: 1, y: 2 };
    const obj2 = { x: 1, y: 2, z: 3 };
    const objects = [obj1, obj2];
    let sum = 0;
    for (let i = 0; i < 100; i++) {
        sum += objects[i % 2].x;
    }
    return sum;
});

// Polymorphic get: 4 shapes (at the limit of current cache)
benchmark("get-polymorphic-4", 1000000, () => {
    const obj1 = { x: 1 };
    const obj2 = { x: 1, y: 2 };
    const obj3 = { x: 1, y: 2, z: 3 };
    const obj4 = { x: 1, y: 2, z: 3, w: 4 };
    const objects = [obj1, obj2, obj3, obj4];
    let sum = 0;
    for (let i = 0; i < 100; i++) {
        sum += objects[i % 4].x;
    }
    return sum;
});

// Megamorphic get: 10 shapes (exceeds current cache)
benchmark("get-megamorphic-10", 1000000, () => {
    const objects = [];
    for (let i = 0; i < 10; i++) {
        const obj = { x: i };
        for (let j = 0; j < i; j++) {
            obj["prop" + j] = j;
        }
        objects.push(obj);
    }
    let sum = 0;
    for (let i = 0; i < 100; i++) {
        sum += objects[i % 10].x;
    }
    return sum;
});

// Megamorphic get: 50 shapes (heavily megamorphic)
benchmark("get-megamorphic-50", 500000, () => {
    const objects = [];
    for (let i = 0; i < 50; i++) {
        const obj = { x: i };
        for (let j = 0; j < i; j++) {
            obj["prop" + j] = j;
        }
        objects.push(obj);
    }
    let sum = 0;
    for (let i = 0; i < 100; i++) {
        sum += objects[i % 50].x;
    }
    return sum;
});

console.log();

// ====== PUT PROPERTY BENCHMARKS ======

console.log("=== PUT PROPERTY BENCHMARKS ===\n");

// Monomorphic put: single shape
benchmark("put-monomorphic", 1000000, () => {
    const obj = { x: 0, y: 0, z: 0 };
    for (let i = 0; i < 100; i++) {
        obj.x = i;
    }
});

// Polymorphic put: 2 shapes
benchmark("put-polymorphic-2", 1000000, () => {
    const obj1 = { x: 0, y: 0 };
    const obj2 = { x: 0, y: 0, z: 0 };
    const objects = [obj1, obj2];
    for (let i = 0; i < 100; i++) {
        objects[i % 2].x = i;
    }
});

// Polymorphic put: 4 shapes
benchmark("put-polymorphic-4", 1000000, () => {
    const obj1 = { x: 0 };
    const obj2 = { x: 0, y: 0 };
    const obj3 = { x: 0, y: 0, z: 0 };
    const obj4 = { x: 0, y: 0, z: 0, w: 0 };
    const objects = [obj1, obj2, obj3, obj4];
    for (let i = 0; i < 100; i++) {
        objects[i % 4].x = i;
    }
});

// Megamorphic put: 10 shapes
benchmark("put-megamorphic-10", 1000000, () => {
    const objects = [];
    for (let i = 0; i < 10; i++) {
        const obj = { x: 0 };
        for (let j = 0; j < i; j++) {
            obj["prop" + j] = 0;
        }
        objects.push(obj);
    }
    for (let i = 0; i < 100; i++) {
        objects[i % 10].x = i;
    }
});

// Megamorphic put: 50 shapes
benchmark("put-megamorphic-50", 500000, () => {
    const objects = [];
    for (let i = 0; i < 50; i++) {
        const obj = { x: 0 };
        for (let j = 0; j < i; j++) {
            obj["prop" + j] = 0;
        }
        objects.push(obj);
    }
    for (let i = 0; i < 100; i++) {
        objects[i % 50].x = i;
    }
});

console.log();

// ====== MIXED ACCESS PATTERNS ======

console.log("=== MIXED ACCESS PATTERNS ===\n");

// Mixed get/put megamorphic
benchmark("mixed-megamorphic-20", 500000, () => {
    const objects = [];
    for (let i = 0; i < 20; i++) {
        const obj = { x: i, y: i * 2 };
        for (let j = 0; j < i; j++) {
            obj["prop" + j] = j;
        }
        objects.push(obj);
    }
    let sum = 0;
    for (let i = 0; i < 100; i++) {
        const obj = objects[i % 20];
        sum += obj.x;
        obj.y = i;
        sum += obj.y;
    }
    return sum;
});

// Prototype chain megamorphic
benchmark("prototype-megamorphic-10", 500000, () => {
    const proto = { inherited: 42 };
    const objects = [];
    for (let i = 0; i < 10; i++) {
        const obj = Object.create(proto);
        obj.own = i;
        for (let j = 0; j < i; j++) {
            obj["prop" + j] = j;
        }
        objects.push(obj);
    }
    let sum = 0;
    for (let i = 0; i < 100; i++) {
        sum += objects[i % 10].inherited;
    }
    return sum;
});

console.log("\n=== BASELINE COMPLETE ===");
