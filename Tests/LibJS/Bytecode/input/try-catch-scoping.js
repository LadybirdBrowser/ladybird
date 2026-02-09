function tryCatchWithBlocks() {
    let x = 1;
    try {
        let y = 2;
        throw y;
    } catch (e) {
        let z = 3;
        console.log(x + e + z);
    }
}
tryCatchWithBlocks();

function tryCatchFinallyWithBlocks() {
    let x = 1;
    try {
        let y = 2;
        throw y;
    } catch (e) {
        console.log(e);
    } finally {
        let z = 3;
        console.log(z);
    }
}
tryCatchFinallyWithBlocks();
