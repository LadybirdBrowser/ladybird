// A local that a TDZ check reads is emptied at the start of each iteration of the innermost
// loop around its scope. A local without such a check costs nothing.

function checked(list) {
    for (let i = 0; i < list.length; i++) {
        list[i](() => 0, typeof value);
        let value = i;
        let plain = value + 1;
        list[i](plain);
    }
}

function nested(list) {
    while (list.length) {
        {
            for (const item of list) item(typeof value);
            let value = list.pop();
        }
    }
}

checked([]);
nested([]);
