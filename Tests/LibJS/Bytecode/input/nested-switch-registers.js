function work() {}

function nested(value0, value1, value2, value3, value4, value5) {
    switch (value5) {
    case 1:
        switch (value4) {
        case 1:
            switch (value3) {
            case 1:
                switch (value2) {
                case 1:
                    switch (value1) {
                    case 1:
                        switch (value0) {
                        case 1:
                            work();
                            break;
                        default:
                            work();
                        }
                        break;
                    default:
                        work();
                    }
                    break;
                default:
                    work();
                }
                break;
            default:
                work();
            }
            break;
        default:
            work();
        }
        break;
    default:
        work();
    }
}

function scoped(value) {
    switch (value) {
    case 1:
        let captured = value;
        return () => captured;
    }
}

nested(1, 1, 1, 1, 1, 1);
scoped(1);
