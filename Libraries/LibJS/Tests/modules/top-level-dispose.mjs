export let passed = false;
let failed = false;

if (passed)
    failed = true;

using a = { [Symbol.dispose]() { if (!failed) passed = true; } }

if (passed)
    failed = true;

failed = true;
// Should trigger before
using b = { [Symbol.dispose]() { if (!passed) failed = false; } }
