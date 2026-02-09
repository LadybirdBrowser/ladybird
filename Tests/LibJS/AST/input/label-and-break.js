// Labels don't create bindings, but blocks under labels do.
function labeled_block() {
    let x = 1;
    outer: {
        let y = 2;
        break outer;
    }
    return x;
}

// Labeled loop with continue.
function labeled_loop() {
    let sum = 0;
    outer: for (let i = 0; i < 3; i++) {
        for (let j = 0; j < 3; j++) {
            if (j === 1) continue outer;
            sum = sum + j;
        }
    }
    return sum;
}
