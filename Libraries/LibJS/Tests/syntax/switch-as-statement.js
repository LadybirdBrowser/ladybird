describe("switch statement is a valid statement and gets executed", () => {
    test("switch statement in a block", () => {
        let hit = false;
        {
            switch (true) {
                case true:
                    hit = true;
            }
            expect(hit).toBeTrue();
        }
    });

    test("switch statement in an if statement when true", () => {
        let hit = false;
        var a = true;
        if (a)
            switch (true) {
                case true:
                    hit = true;
            }
        else
            switch (true) {
                case true:
                    expect().fail();
            }

        expect(hit).toBeTrue();
    });

    test("switch statement in an if statement when false", () => {
        let hit = false;
        var a = false;
        if (a)
            switch (a) {
                default:
                    expect().fail();
            }
        else
            switch (a) {
                default:
                    hit = true;
            }

        expect(hit).toBeTrue();
    });

    test("switch statement in an while statement", () => {
        var a = 0;
        var loops = 0;
        while (a < 1 && loops++ < 5)
            switch (a) {
                case 0:
                    a = 1;
            }

        expect(a).toBe(1);
    });

    test("switch statement in an for statement", () => {
        var loops = 0;
        for (let a = 0; a < 1 && loops++ < 5; )
            switch (a) {
                case 0:
                    a = 1;
            }

        expect(loops).toBe(1);
    });
});
