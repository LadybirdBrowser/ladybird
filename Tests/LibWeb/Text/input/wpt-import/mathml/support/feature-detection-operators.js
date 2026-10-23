// This is a helper for MathML feature detection.
// It is indented to be used to prevent false negative test results.
// This adds operator-specific feature detections.

Object.assign(MathMLFeatureDetection, {
    "has_operator_lspace/rspace": async function() {
        return this.has_operator_spacing();
    },

    "has_operator_movablelimits": async function() {
        return this.has_movablelimits();
    },

    "has_operator_largeop": async function() {
        if (!this.hasOwnProperty("_has_operator_largeop")) {
            document.body.insertAdjacentHTML("beforeend", "\
<math style='font: 10px HasOperatorLargeopTestFont;'\
      displaystyle='true'>\
  <mo largeop='false' stretchy='false' symmetric='false'>&#x2AFF;</mo>\
  <mo largeop='true' stretchy='false' symmetric='false'>&#x2AFF;</mo>\
</math>");
            let font_face = new FontFace('HasOperatorLargeopTestFont',
                'url(/fonts/math/largeop-displayoperatorminheight5000.woff)');
            document.fonts.add(font_face);
            await font_face.load();
            var math = document.body.lastElementChild;
            var mo = math.getElementsByTagName("mo");
            this._has_operator_largeop =
                (mo[1].getBoundingClientRect().height >
                 mo[0].getBoundingClientRect().height);
            document.body.removeChild(math);
            document.fonts.delete(font_face);
        }
        return this._has_operator_largeop;
    },

    "has_operator_stretchy": async function() {
        if (!this.hasOwnProperty("_has_operator_stretchy")) {
            document.body.insertAdjacentHTML("beforeend", "\
<math style='font: 10px HasOperatorStretchyTestFont;'>\
  <mrow>\
    <mo stretchy='false' largeop='false' symmetric='false'>&#x2AFF;</mo>\
    <mo stretchy='true' largeop='false' symmetric='false'>&#x2AFF;</mo>\
    <mspace style='background: black;' width='1px' height='2em'></mspace>\
  </mrow>\
</math>");
            let font_face = new FontFace('HasOperatorLargeopTestFont',
                'url(/fonts/math/largeop-displayoperatorminheight5000.woff)');
            document.fonts.add(font_face);
            await font_face.load();
            var math = document.body.lastElementChild;
            var mo = math.getElementsByTagName("mo");
            this._has_operator_stretchy =
                (mo[1].getBoundingClientRect().height >
                 mo[0].getBoundingClientRect().height);
            document.body.removeChild(math);
            document.fonts.delete(font_face);
        }
        return this._has_operator_stretchy;
    },

    "has_operator_symmetric": async function() {
        if (!this.hasOwnProperty("_has_operator_symmetric")) {
            document.body.insertAdjacentHTML("beforeend", "\
<math style='font: 10px HasOperatorSymmetricTestFont;'>\
  <mrow>\
    <mo stretchy='true' largeop='false' symmetric='false'>&#x2AFF;</mo>\
    <mo stretchy='true' largeop='false' symmetric='true'>&#x2AFF;</mo>\
    <mspace style='background: black;' width='1px' height='2em'></mspace>\
  </mrow>\
</math>");
            let font_face = new FontFace('HasOperatorLargeopTestFont',
                'url(/fonts/math/largeop-displayoperatorminheight5000.woff)');
            document.fonts.add(font_face);
            await font_face.load();
            var math = document.body.lastElementChild;
            var mo = math.getElementsByTagName("mo");
            this._has_operator_symmetric =
                (mo[1].getBoundingClientRect().height >
                 mo[0].getBoundingClientRect().height);
            document.body.removeChild(math);
            document.fonts.delete(font_face);
        }
        return this._has_operator_symmetric;
    },
});
