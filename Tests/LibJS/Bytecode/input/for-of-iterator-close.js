function forOfBreak() {
    for (const x of [1, 2, 3]) {
        if (x === 2) break;
    }
}
forOfBreak();

function forOfReturn() {
    for (const x of [1, 2, 3]) {
        return x;
    }
}
forOfReturn();

async function forAwaitOfBreak(iter) {
    for await (const x of iter) {
        if (x === 2) break;
    }
}
forAwaitOfBreak([1, 2, 3]);
