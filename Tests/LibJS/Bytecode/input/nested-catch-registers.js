function work() {}
function recover() {}

function nested() {
    try {
        work();
    } catch {
        try {
            work();
        } catch {
            try {
                work();
            } catch {
                try {
                    work();
                } catch {
                    try {
                        work();
                    } catch {
                        try {
                            work();
                        } catch {
                            recover();
                        }
                    }
                }
            }
        }
    }
}

function bound() {
    try {
        work();
    } catch (error) {
        try {
            work();
        } catch ({ message }) {
            recover(error, message);
        }
    }
}

nested();
bound();
