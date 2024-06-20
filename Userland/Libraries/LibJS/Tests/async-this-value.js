test("this value in async function", () => {
    function X() {
        this.boog = async () => {
            this.f = await null;
        };
    }
    new X().boog();
});
