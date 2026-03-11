function twoLocals() {
    let a = 1;
    let b = 2;
    return a + b;
}

function threeLocals() {
    let a = 1;
    let b = 2;
    let c = 3;
    return a + b + c;
}

console.log(twoLocals());
console.log(threeLocals());
