function calculate() {
    let first = 20;
    let second = 22;
    return first + second;
}

calculate();

try {
    throw new Error("handled");
} catch {}
