// Simple megamorphic inline cache correctness test

console.log("=== Megamorphic IC Correctness Test ===");

let passed = 0;
let failed = 0;

function assert(condition, message) {
    if (condition) {
        passed++;
    } else {
        console.log("FAIL: " + message);
        failed++;
    }
}

// Test 1: Megamorphic GET - own properties
{
    let objs = [];
    for (let i = 0; i < 20; i++) {
        let obj = {};
        obj["prop" + i] = i * 10;
        objs.push(obj);
    }

    let sum = 0;
    for (let i = 0; i < 100; i++) {
        for (let j = 0; j < 20; j++) {
            sum += objs[j]["prop" + j];
        }
    }

    assert(sum === 190000, "Megamorphic GET: expected 190000, got " + sum);
}

// Test 2: Megamorphic PUT
{
    let objs = [];
    for (let i = 0; i < 20; i++) {
        let obj = {};
        obj["value" + i] = 0;
        objs.push(obj);
    }

    for (let i = 0; i < 100; i++) {
        for (let j = 0; j < 20; j++) {
            objs[j]["value" + j] = i;
        }
    }

    let correct = true;
    for (let j = 0; j < 20; j++) {
        if (objs[j]["value" + j] !== 99) {
            correct = false;
            break;
        }
    }

    assert(correct, "Megamorphic PUT: all values should be 99");
}

// Test 3: Prototype chain with mega morphic access
{
    let proto = { sharedProp: 42 };
    let objs = [];
    for (let i = 0; i < 15; i++) {
        let obj = Object.create(proto);
        obj["ownProp" + i] = i;
        objs.push(obj);
    }

    let sum = 0;
    for (let i = 0; i < 100; i++) {
        for (let j = 0; j < 15; j++) {
            sum += objs[j].sharedProp;
        }
    }

    assert(sum === 63000, "Prototype chain megamorphic: expected 63000, got " + sum);
}

// Test 4: Shape transitions don't break cache
{
    let obj = { a: 1, b: 2 };
    let sum = 0;

    for (let i = 0; i < 1000; i++) {
        sum += obj.a;
        sum += obj.b;
    }

    // Add new property (shape transition)
    obj.c = 3;

    for (let i = 0; i < 1000; i++) {
        sum += obj.a;
        sum += obj.b;
        sum += obj.c;
    }

    assert(sum === 9000, "Shape transitions: expected 9000, got " + sum);
}

// Test 5: Deletions invalidate cache
{
    let obj = { x: 10, y: 20 };
    let val1 = obj.x;
    delete obj.x;
    let val2 = obj.x;

    assert(val1 === 10 && val2 === undefined, "Deletion: x should be 10 then undefined");
}

// Test 6: Accessors (getters/setters)
{
    let backingValue = 0;
    let obj = {
        get value() { return backingValue; },
        set value(v) { backingValue = v * 2; }
    };

    obj.value = 5;
    let result = obj.value;

    assert(result === 10, "Accessors: expected 10, got " + result);
}

// Test 7: Mixed megamorphic access (different property names)
{
    let objs = [];
    for (let i = 0; i < 10; i++) {
        let obj = {};
        for (let j = 0; j < 5; j++) {
            obj["field" + j] = i + j;
        }
        objs.push(obj);
    }

    let sum = 0;
    for (let iter = 0; iter < 50; iter++) {
        for (let i = 0; i < 10; i++) {
            for (let j = 0; j < 5; j++) {
                sum += objs[i]["field" + j];
            }
        }
    }

    assert(sum === 16250, "Mixed megamorphic: expected 16250, got " + sum);
}

console.log("=== Results ===");
console.log("Passed: " + passed);
console.log("Failed: " + failed);

if (failed === 0) {
    console.log("✅ ALL TESTS PASSED!");
} else {
    console.log("❌ SOME TESTS FAILED");
}
