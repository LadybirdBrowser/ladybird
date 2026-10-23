// The top-level code of a module is compiled once its loops run long, with its module bindings, exported ones too,
// its undefined this value, closures it creates, and exceptions it throws and catches, and leaves to the interpreter
// for what it had not seen.
export let total = 0;
let count = 0;
const results = [];
const holes = new Array(5000);
function add(value) {
    total += value;
    return total;
}
for (let i = 0; i < 20000; ++i) {
    count++;
    add(i % 7);
    if (this !== undefined) throw new Error("wrong this");
    if (i % 1000 === 0) results.push(() => i + count);
    try {
        if (i === 15000) throw new Error(`thrown at ${i}`);
    } catch (e) {
        results.push(e.message);
    }
    if (i === 17000) total += 0.5;
    if (i === 18000) results.push(typeof import.meta);
}
let undefinedCount = 0;
for (const value of holes) {
    if (value === undefined) ++undefinedCount;
}
let text = "";
for (let i = 0; i < 3000; ++i) text += String.fromCharCode(97 + (i % 26));
print(
    total,
    count,
    results.length,
    results[3](),
    results.filter(r => typeof r !== "function").join(),
    undefinedCount,
    text.length
);
