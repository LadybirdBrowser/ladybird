// META: script=/css/support/color-testcommon.js

// While assert_equals is fine for hex, it's not for hexalpha, p3, and p3alpha. We use the default
// epsilon of 0.0001.
const assert_colors = set_up_fuzzy_color_test();

[
  {
    value: "",
    hex: "#000000",
    hexalpha: "color(srgb 0 0 0)",
    p3: "color(display-p3 0 0 0)",
    p3alpha: "color(display-p3 0 0 0)"
  },
  {
    hex: "#000000",
    hexalpha: "color(srgb 0 0 0)",
    p3: "color(display-p3 0 0 0)",
    p3alpha: "color(display-p3 0 0 0)"
  },
  {
    value: "#ffffff",
    hex: "#ffffff",
    hexalpha: "color(srgb 1 1 1)",
    p3: "color(display-p3 1 1 1)",
    p3alpha: "color(display-p3 1 1 1)"
  },
  {
    value: "#ffffff08",
    hex: "#ffffff",
    hexalpha: "color(srgb 1 1 1 / 0.031373)",
    p3: "color(display-p3 1 1 1)",
    p3alpha: "color(display-p3 1 1 1 / 0.031373)"
  },
  {
    value: "#FFFFFF",
    hex: "#ffffff",
    hexalpha: "color(srgb 1 1 1)",
    p3: "color(display-p3 1 1 1)",
    p3alpha: "color(display-p3 1 1 1)"
  },
  {
    value: "#0F0F0F",
    hex: "#0f0f0f",
    hexalpha: "color(srgb 0.058824 0.058824 0.058824)",
    p3: "color(display-p3 0.058824 0.058824 0.058824)",
    p3alpha: "color(display-p3 0.058824 0.058824 0.058824)"
  },
  {
    value: "#fff",
    hex: "#ffffff",
    hexalpha: "color(srgb 1 1 1)",
    p3: "color(display-p3 1 1 1)",
    p3alpha: "color(display-p3 1 1 1)"
  },
  {
    value: "fffffff",
    hex: "#000000",
    hexalpha: "color(srgb 0 0 0)",
    p3: "color(display-p3 0 0 0)",
    p3alpha: "color(display-p3 0 0 0)"
  },
  {
    value: "#gggggg",
    hex: "#000000",
    hexalpha: "color(srgb 0 0 0)",
    p3: "color(display-p3 0 0 0)",
    p3alpha: "color(display-p3 0 0 0)"
  },
  {
    value: "foobar",
    hex: "#000000",
    hexalpha: "color(srgb 0 0 0)",
    p3: "color(display-p3 0 0 0)",
    p3alpha: "color(display-p3 0 0 0)"
  },
  {
    value: "#ffffff\u0000",
    hex: "#000000",
    hexalpha: "color(srgb 0 0 0)",
    p3: "color(display-p3 0 0 0)",
    p3alpha: "color(display-p3 0 0 0)"
  },
  {
    value: "#ffffff;",
    hex: "#000000",
    hexalpha: "color(srgb 0 0 0)",
    p3: "color(display-p3 0 0 0)",
    p3alpha: "color(display-p3 0 0 0)"
  },
  {
    value: " #ffffff",
    hex: "#ffffff",
    hexalpha: "color(srgb 1 1 1)",
    p3: "color(display-p3 1 1 1)",
    p3alpha: "color(display-p3 1 1 1)"
  },
  {
    value: "#ffffff ",
    hex: "#ffffff",
    hexalpha: "color(srgb 1 1 1)",
    p3: "color(display-p3 1 1 1)",
    p3alpha: "color(display-p3 1 1 1)"
  },
  {
    value: " #ffffff ",
    hex: "#ffffff",
    hexalpha: "color(srgb 1 1 1)",
    p3: "color(display-p3 1 1 1)",
    p3alpha: "color(display-p3 1 1 1)"
  },
  {
    value: "crimson",
    hex: "#dc143c",
    hexalpha: "color(srgb 0.862745 0.078431 0.235294)",
    p3: "color(display-p3 0.791711 0.191507 0.257367)",
    p3alpha: "color(display-p3 0.791711 0.191507 0.257367)"
  },
  {
    value: "bisque",
    hex: "#ffe4c4",
    hexalpha: "color(srgb 1 0.894118 0.768627)",
    p3: "color(display-p3 0.982297 0.8979 0.783276)",
    p3alpha: "color(display-p3 0.982297 0.8979 0.783276)"
  },
  {
    value: "currentColor",
    hex: "#000000",
    hexalpha: "color(srgb 0 0 0)",
    p3: "color(display-p3 0 0 0)",
    p3alpha: "color(display-p3 0 0 0)"
  },
  {
    value: "transparent",
    hex: "#000000",
    hexalpha: "color(srgb 0 0 0 / 0)",
    p3: "color(display-p3 0 0 0)",
    p3alpha: "color(display-p3 0 0 0 / 0)"
  },
  {
    value: "inherit",
    hex: "#000000",
    hexalpha: "color(srgb 0 0 0)",
    p3: "color(display-p3 0 0 0)",
    p3alpha: "color(display-p3 0 0 0)"
  },
  {
    value: "rgb(1,1,1)",
    hex: "#010101",
    hexalpha: "color(srgb 0.003922 0.003922 0.003922)",
    p3: "color(display-p3 0.003922 0.003922 0.003922)",
    p3alpha: "color(display-p3 0.003922 0.003922 0.003922)"
  },
  {
    value: "rgb(1,1,1,1)",
    hex: "#010101",
    hexalpha: "color(srgb 0.003922 0.003922 0.003922)",
    p3: "color(display-p3 0.003922 0.003922 0.003922)",
    p3alpha: "color(display-p3 0.003922 0.003922 0.003922)"
  },
  {
    value: "rgb(1,1,1,0.5)",
    hex: "#010101",
    hexalpha: "color(srgb 0.003922 0.003922 0.003922 / 0.501961)",
    p3: "color(display-p3 0.003922 0.003922 0.003922)",
    p3alpha: "color(display-p3 0.003922 0.003922 0.003922 / 0.501961)"
  },
  {
    value: "#FFFFF\u1F4A9",
    hex: "#000000",
    hexalpha: "color(srgb 0 0 0)",
    p3: "color(display-p3 0 0 0)",
    p3alpha: "color(display-p3 0 0 0)"
  }
].forEach(({ value, hex, hexalpha, p3, p3alpha }) => {
  ["limited-srgb", "display-p3"].forEach(colorSpace => {
    [false, true].forEach(alpha => {
      const nameValue = value === "" ? "the empty string" : value === undefined ? "no value" : "'" + value + "'";
      test(() => {
        const input = document.createElement("input");
        input.type = "color";
        if (value !== undefined) {
          input.setAttribute("value", value);
        }
        assert_equals(input.value, hex, "value is hex");
        input.colorSpace = colorSpace;
        assert_equals(input.colorSpace, colorSpace, "color space");
        if (colorSpace === "limited-srgb") {
          assert_equals(input.value, hex, "value is hex");
        } else {
          assert_colors(input.value, p3);
        }
        input.alpha = alpha;
        assert_equals(input.alpha, alpha, "alpha");
        if (colorSpace === "limited-srgb" && !alpha) {
          assert_equals(input.value, hex, "value is hex");
        } else if (colorSpace === "limited-srgb" && alpha) {
          assert_colors(input.value, hexalpha);
        } else if (colorSpace === "display-p3" && !alpha) {
          assert_colors(input.value, p3);
        } else {
          assert_colors(input.value, p3alpha);
        }
      }, `Testing ${nameValue} with color space '${colorSpace}' and ${alpha ? 'with' : ' without'} alpha (setAttribute("value"))`);

      test(() => {
        const input = document.createElement("input");
        input.type = "color";
        // In this test we set alpha before we set value to avoid the value sanitization algorithm
        // taking away the alpha channel from the input.
        input.alpha = true;
        if (value !== undefined) {
          input.value = value;
        }
        assert_colors(input.value, hexalpha);
        input.colorSpace = colorSpace;
        assert_equals(input.colorSpace, colorSpace, "color space");
        if (colorSpace === "limited-srgb") {
          assert_colors(input.value, hexalpha);
        } else {
          assert_colors(input.value, p3alpha);
        }
        input.alpha = alpha;
        assert_equals(input.alpha, alpha, "alpha");
        if (colorSpace === "limited-srgb" && !alpha) {
          assert_equals(input.value, hex, "value is hex");
        } else if (colorSpace === "limited-srgb" && alpha) {
          assert_colors(input.value, hexalpha);
        } else if (colorSpace === "display-p3" && !alpha) {
          assert_colors(input.value, p3);
        } else {
          assert_colors(input.value, p3alpha);
        }
      }, `Testing ${nameValue} with color space '${colorSpace}' and ${alpha ? 'with' : ' without'} alpha (value)`);
    });
  });
});

test(() => {
  const input = document.createElement("input");
  input.type = "color";
  input.alpha = true;
  input.colorSpace = "display-p3";
  input.value = "color(display-p3 3 none .2 / .6)";
  assert_equals(input.value, "color(display-p3 3 0 0.2 / 0.6)");
}, "Display P3 colors can be out-of-bounds");

test(() => {
  const input = document.createElement("input");
  input.type = "color";
  input.value = "white";
  input.setAttribute("colorspace", "display-p3");
  assert_equals(input.value, "color(display-p3 1 1 1)");
}, "Setting colorspace by setAttribute should update the value");

test(() => {
  const input = document.createElement("input");
  input.type = "color";
  input.value = "blue";
  input.setAttribute("alpha", "");
  assert_equals(input.value, "color(srgb 0 0 1)");
}, "Setting alpha by setAttribute should update the value");

// When the value is not dirty, changing colorspace/alpha re-sanitizes from the
// original `value` content attribute rather than the current value, so
// information dropped under the previous attributes (such as an alpha channel)
// is recovered. See https://github.com/whatwg/html/issues/12057.
test(() => {
  const input = document.createElement("input");
  input.type = "color";
  // Setting the content attribute (rather than the IDL value) leaves the value
  // non-dirty. Without the alpha attribute the value sanitizes to opaque.
  input.setAttribute("value", "#ffffff08");
  assert_equals(input.value, "#ffffff");
  input.setAttribute("alpha", "");
  // Reparsing the original "#ffffff08" recovers the alpha channel; reparsing the
  // now-opaque current value would instead give "color(srgb 1 1 1)".
  assert_colors(input.value, "color(srgb 1 1 1 / 0.031373)");
}, "Adding alpha reparses the original value attribute for a non-dirty value");
