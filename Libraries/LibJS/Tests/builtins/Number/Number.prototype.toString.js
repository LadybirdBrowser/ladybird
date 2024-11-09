describe("correct behavior", () => {
    test("length", () => {
        expect(Number.prototype.toString).toHaveLength(1);
    });

    test("basic functionality", () => {
        [
            [+0, "0"],
            [-0, "0"],
            [Infinity, "Infinity"],
            [-Infinity, "-Infinity"],
            [NaN, "NaN"],
            [12, "12"],
            [93465, "93465"],
            [358000, "358000"],
            // Numbers above 2 ** 31 - 1 (Issue #3931)
            [2147483648, "2147483648"], // 2 ** 31
            [4294967295, "4294967295"], // 2 ** 32 - 1
            [18014398509481984, "18014398509481984"], // 2 ** 54
        ].forEach(testCase => {
            expect(testCase[0].toString()).toBe(testCase[1]);
        });
    });

    test("radix", () => {
        let number = 7857632;

        [
            [2, "11101111110010111100000"],
            [3, "112210012122102"],
            [4, "131332113200"],
            [5, "4002421012"],
            [6, "440225532"],
            [7, "123534356"],
            [8, "35762740"],
            [9, "15705572"],
            [10, "7857632"],
            [11, "4487612"],
            [12, "276b2a8"],
            [13, "18216b3"],
            [14, "10877d6"],
            [15, "a532c2"],
            [16, "77e5e0"],
            [17, "59160b"],
            [18, "42f5h2"],
            [19, "335b5b"],
            [20, "29241c"],
            [21, "1j89fk"],
            [22, "1bbkh2"],
            [23, "151ih4"],
            [24, "ng9h8"],
            [25, "k2m57"],
            [26, "h51ig"],
            [27, "el5hb"],
            [28, "clqdk"],
            [29, "b355o"],
            [30, "9l0l2"],
            [31, "8fng0"],
            [32, "7fpf0"],
            [33, "6klf2"],
            [34, "5tv8s"],
            [35, "589dr"],
            [36, "4oezk"],
        ].forEach(testCase => {
            expect(number.toString(testCase[0])).toBe(testCase[1]);
        });
    });

    test("decimal radix gets converted to int", () => {
        expect((30).toString(10.1)).toBe("30");
        expect((30).toString(10.9)).toBe("30");
    });

    test("extremely large number with radix", () => {
        // NOTE: The answers change depending on the engine, so the important thing here is that it doesn't crash
        //       and is good enough.
        // Example number generated from an ID generator in Raygun4js: https://github.com/MindscapeHQ/raygun4js/blob/777ba6e3cdede42011aa0b7f019473de6380009b/src/raygun.rum/index.js#L1204
        const number = 2.2171010912173817e51;

        [
            [
                2,
                "101111011010000000010001011010101110000101111100111010000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
            ],
            [
                3,
                "122200222200200002020000221212111212020212021222122122201112112001202200202210122011020121220102211210120110",
            ],
            [
                4,
                "11323100002023111300233213100000000000000000000000000000000000000000000000000000000000",
            ],
            [5, "20213333043100433442123113244334300004432332001224240433304410013422100304"],
            [6, "550003524220341511224004404200404220224024020444420402442220220420"],
            [7, "4235625635040541506562064422461122351652161506062250163366335"],
            [8, "573200213256057472000000000000000000000000000000000000000"],
            [9, "580880602200855367001288271433120018026801542154811843"],
            [10, "2.2171010912173817e+51"],
            [11, "20941975227252065997040823726473715831623804aa6726"],
            [12, "426276185347a25840488044080484880408844840480440"],
            [13, "136bc76718505c84cb00730624426c569759715872b8b70"],
            [14, "835c3959100c338282668c622a8a060a24602aa6a66cc"],
            [15, "5e1ae5933690a58aad6a9546513bbbd49305ec050549"],
            [16, "5ed008b570be7400000000000000000000000000000"],
            [17, "7f3da30c676ag43gc65022d17gae829c8790d1d479"],
            [18, "dba188ded00ha8a6c2800e808c20aagggg4g4822c"],
            [19, "1af846i1f7ge0bfb7b4c3f1abd5a70g3bhd592fb6"],
            [20, "40d316h12f73ec848404c8c0g4400gc48c400804"],
            [21, "cd59ci682hih768393c4d1kc5757jkgg918b2dc"],
            [22, "239f104idd3dikgg6e4eccg66aee26a0kc2kac6"],
            [23, "93f4b0317667h2gm0gka6ia4h9emj7b6aef4ig"],
            [24, "1lca597424748g080gggg80g08gg8880880g80"],
            [25, "abii4g0njm70ejj3c39lajgi5l3bdf6gl25ne"],
            [26, "2e8foaj1lec80c86cck264caii282m6m8e6a0"],
            [27, "hiqii260pnbeqe894nd351nqpo5nm3j45jj3"],
            [28, "4qmgqk0rk788oog4c8k0k0cg8o04cocckckc"],
            [29, "1d2nmd4bln2jmf0k72sl2ejhmj7m8rq70flk"],
            [30, "d8onrgfc6kaik26a4oqmee4ic0ksi0k0k2o"],
            [31, "4b4u5lb8fb8ttacfifj5g1iapimkppcehrq"],
            [32, "1fd025le2v7800000000000000000000000"],
            [33, "h5mgn3kr83kucw13c6102w7n5loohbfct6"],
            [34, "6e0244vn6ba0gw8ucgkmuwgk4oi02g8icq"],
            [35, "2g7ohyr3v1t7c0jrq621aw56kflrj71pfj"],
            [36, "z03wqcmb7dw00g80wow0sksk484wgswsc"],
        ].forEach(([radix, result]) => {
            expect(number.toString(radix)).toBe(result);
        });
    });
});

describe("errors", () => {
    test("must be called with numeric |this|", () => {
        [true, [], {}, Symbol("foo"), "bar", 1n].forEach(value => {
            expect(() => Number.prototype.toString.call(value)).toThrowWithMessage(
                TypeError,
                "Not an object of type Number"
            );
        });
    });

    test("radix RangeError", () => {
        [0, 1, 37, 100].forEach(value => {
            expect(() => (0).toString(value)).toThrow(RangeError);
        });
    });
});
