function try_catch() {
    let x = 1;
    try {
        let y = 2;
        return x + y;
    } catch (e) {
        return e;
    }
}

function catch_parameter() {
    try {
        throw 1;
    } catch (err) {
        let msg = err;
        return msg;
    }
}

function try_catch_finally() {
    let result = 0;
    try {
        result = 1;
    } catch (e) {
        result = 2;
    } finally {
        result = result + 10;
    }
    return result;
}
