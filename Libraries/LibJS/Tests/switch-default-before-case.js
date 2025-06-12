test("default clause before matching case clause", () => {
    switch (1 + 2) {
        default:
            expect().fail();
            break;
        case 3:
            return;
    }
});
