(function(root){

/*
 * General Value Types definition
 * they return an object of arrays of type { <name>: [<start-value>, <end-value>], ... }
 */
var values = {
    'length' : function() {
        // http://www.w3.org/TR/css3-values/#lengths
        return {
            // CSS Values and Module Level 3
            // ch: ['1ch', '10ch'],
            // rem: ['1rem', '10rem'],
            // vw: ['1vw', '10vw'],
            // vh: ['1vh', '10vh'],
            // vmin: ['1vmin', '10vmin'],
            // vmax: ['1vmax', '10vmax'],
            // CSS Values and Module Level 2
            pt: ['1pt', '10pt'],
            pc: ['1pc', '10pc'],
            px: ['1px', '10px'],
            // CSS Values and Module Level 1
            em: ['1em', '10em'],
            ex: ['1ex', '10ex'],
            mm: ['1mm', '10mm'],
            cm: ['1cm', '10cm'],
            'in': ['1in', '10in']
        };
    },
    'length-em': function() {
        return {
            em: ['1.1em', '1.5em']
        };
    },
    'percentage': function() {
        // http://www.w3.org/TR/css3-values/#percentages
        return {
            '%': ['33%', '80%']
        };
    },
    'color': function() {
        // http://www.w3.org/TR/css3-values/#colors
        // http://www.w3.org/TR/css3-color/
        return {
            rgba: ['rgba(100,100,100,1)', 'rgba(10,10,10,0.4)']
        };
    },
    'rectangle': function() {
        // http://www.w3.org/TR/CSS2/visufx.html#value-def-shape
        return {
            rectangle: ['rect(10px,10px,10px,10px)', 'rect(15px,15px,5px,5px)']
        };
    },
    'font-weight': function() {
        // http://www.w3.org/TR/css3-fonts/#font-weight-prop
        return {
            keyword: ["normal", "bold"],
            numeric: ["100", "900"]
        };
    },
    'number': function() {
        // http://www.w3.org/TR/css3-values/#number
        return {
            integer: ["1", "10"],
            decimal: ["1.1", "9.55"]
        };
    },
    'number[0,1]': function() {
        // http://www.w3.org/TR/css3-values/#number
        // applies to [0,1]-ranged properties like opacity
        return {
            "zero-to-one": ["0.2", "0.9"]
        };
    },
    'integer': function() {
        // http://www.w3.org/TR/css3-values/#integer
        return {
            integer: ["1", "10"]
        };
    },
    'shadow': function() {
        // http://www.w3.org/TR/css-text-decor-3/#text-shadow-property
        return {
            shadow: ['rgba(0,0,0,0.1) 5px 6px 7px', 'rgba(10,10,10,0.9) 5px 6px 7px']
        };
    },
    'visibility': function() {
        // http://www.w3.org/TR/CSS2/visufx.html#visibility
        return {
            keyword: ['visible', 'hidden', {discrete: true}]
        };
    },
    // types reqired for non-specified properties
    'border-radius': function() {
        return {
            px: ['1px', '10px'],
            "px-px": ['1px 3px', '10px 13px']
        };
    },
    'image' : function() {
        var prefix = getValueVendorPrefix('background-image', 'linear-gradient(top, hsl(0, 80%, 70%), #bada55)');
        return {
            // Chrome implements this
            url: ['url(support/one.gif)', 'url(support/two.gif)'],
            data: ['url(data:image/gif;base64,R0lGODlhAQABAAD/ACwAAAAAAQABAAACADs=)', 'url(data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==)'],
            // A hunch, as from the spec:
            // http://www.w3.org/TR/css3-transitions/#animatable-types
            // gradient: interpolated via the positions and colors of each stop. They must have the same type (radial or linear) and same number of stops in order to be animated. Note: [CSS3-IMAGES] may extend this definition.
            gradient: [prefix + 'linear-gradient(top, hsl(0, 80%, 70%), #bada55)', prefix + 'linear-gradient(top, #bada55, hsl(0, 80%, 70%))']
        };
    },
    'background-size': function() {
        return {
            keyword: ['cover', 'contain']
        };
    },
    'box-shadow': function() {
        // http://www.w3.org/TR/css3-background/#ltshadowgt
        return {
            shadow: ['60px -16px teal', '60px -16px red']
        };
    },
    'vertical': function() {
        return {
            keyword: ['top', 'bottom']
        };
    },
    'horizontal': function() {
        return {
            keyword: ['left', 'right']
        };
    },
    'font-stretch': function() {
        return {
            keyword: ['condensed', 'expanded']
        };
    },
    'transform': function() {
        return {
            rotate: ['rotate(10deg)', 'rotate(20deg)']
        };
    },
    'position': function() {
        return {
            'static to absolute': ['static', 'absolute', {discrete: true}],
            'relative to absolute': ['relative', 'absolute', {discrete: true}],
            'absolute to fixed': ['absolute', 'fixed', {discrete: true}]
        };
    },
    'display': function() {
        return {
            'static to absolute': ['none', 'block', {discrete: true}],
            'block to inline-block': ['block', 'inline-block', {discrete: true}]
        };
    },
    'object-view-box': function() {
        return {
            inset: ['inset(10% 10% 20% 20%)', 'inset(20% 20% 30% 30%)'],
            rect: ['rect(10px 20px 30px 40px)', 'rect(20px 30px 40px 50px)'],
            xywh: ['xywh(10px 20px 30px 40px)', 'xywh(20px 30px 40px 50px)'],
        };
    }
};

/*
 * Property to Type table
 * (as stated in specification)
 */
var properties = {
    'background-color': ['color'],
    'background-position-x': ['length', 'percentage'],

    'border-top-width': ['length'],
    'border-right-width': ['length'],
    'border-bottom-width': ['length'],
    'border-left-width': ['length'],

    'border-top-color': ['color'],
    'border-right-color': ['color'],
    'border-bottom-color': ['color'],
    'border-left-color': ['color'],

    'padding-bottom': ['length'],
    'padding-left': ['length'],
    'padding-right': ['length'],
    'padding-top': ['length'],

    'margin-bottom': ['length'],
    'margin-left': ['length'],
    'margin-right': ['length'],
    'margin-top': ['length'],

    'height': ['length', 'percentage'],
    'width': ['length', 'percentage'],
    'min-height': ['length', 'percentage'],
    'min-width': ['length', 'percentage'],
    'max-height': ['length', 'percentage'],
    'max-width': ['length', 'percentage'],

    'top': ['length', 'percentage'],
    'right': ['length', 'percentage'],
    'bottom': ['length', 'percentage'],
    'left': ['length', 'percentage'],

    'color': ['color'],
    'font-size': ['length', 'percentage'],
    'font-weight': ['font-weight'],
    'line-height': ['number', 'length', 'percentage'],
    'letter-spacing': ['length'],
    // Note: percentage is Level3 and not implemented anywhere yet
    // https://drafts.csswg.org/css3-text/#word-spacing
    'word-spacing': ['length', 'percentage'],
    'text-indent': ['length', 'percentage'],
    'text-shadow': ['shadow'],

    'outline-color': ['color'],
    // outline-offset <integer> used to be an error in the spec
    'outline-offset': ['length'],
    'outline-width': ['length'],

    'clip': ['rectangle'],

    'vertical-align': ['length', 'percentage'],
    'opacity': ['number[0,1]'],
    'visibility': ['visibility'],
    'z-index': ['integer']
};

/*
 * Property to Type table
 * (missing value-types of specified properties)
 */
var missing_properties = {
    'margin-bottom': ['percentage'],
    'margin-left': ['percentage'],
    'margin-right': ['percentage'],
    'margin-top': ['percentage'],
    'padding-bottom': ['percentage'],
    'padding-left': ['percentage'],
    'padding-right': ['percentage'],
    'padding-top': ['percentage'],
    'vertical-align': ['vertical']
};

/*
 * Property to Type table
 * (properties that haven't been specified but implemented)
 */
var unspecified_properties = {
    // http://oli.jp/2010/css-animatable-properties/
    'border-top-left-radius': ['border-radius'],
    'border-top-right-radius': ['border-radius'],
    'border-bottom-left-radius': ['border-radius'],
    'border-bottom-right-radius': ['border-radius'],
    'background-image': ['image'],
    'background-size': ['background-size'],
    // https://drafts.csswg.org/css3-background/#the-box-shadow
    // Animatable:   yes, except between inner and outer shadows (Transition to/from an absent shadow is a transition to/from ‘0 0 transparent’ or ‘0 0 transparent inset’, as appropriate.)
    'box-shadow': ['box-shadow'],
    'font-size-adjust': ['number'],
    'font-stretch': ['font-stretch'],
    'text-decoration-color': ['color'],
    'column-count': ['integer'],
    'column-gap': ['length'],
    'column-rule-color': ['color'],
    'column-rule-width': ['length'],
    'column-width': ['length'],
    'column-height': ['length'],
    'transform': ['transform'],
    'transform-origin': ['horizontal'],
    'display': ['display'],
    'position': ['position'],
    'object-view-box': ['object-view-box']
};

/*
 * additional styles required to actually render
 * (different browsers expect different environment)
 */
var additional_styles = {
    // all browsers
    'border-top-width': {'border-top-style' : 'solid'},
    'border-right-width': {'border-right-style' : 'solid'},
    'border-bottom-width': {'border-bottom-style' : 'solid'},
    'border-left-width': {'border-left-style' : 'solid'},
    'top': {'position': 'absolute'},
    'right': {'position': 'absolute'},
    'bottom': {'position': 'absolute'},
    'left': {'position': 'absolute'},
    'z-index': {'position': 'absolute'},
    'outline-offset': {'outline-style': 'solid'},
    'outline-width': {'outline-style': 'solid'},
    'word-spacing': {'width': '100px', 'height': '100px'},
    // unspecified properties
    'column-rule-width': {'column-rule-style': 'solid'},
    'position': {'width': '50px', 'height': '50px', top: '10px', left: '50px'}
};

/*
 * additional styles required *on the parent* to actually render
 * (different browsers expect different environment)
 */
var parent_styles = {
    'border-top-width': {'border-top-style' : 'solid'},
    'border-right-width': {'border-right-style' : 'solid'},
    'border-bottom-width': {'border-bottom-style' : 'solid'},
    'border-left-width': {'border-left-style' : 'solid'},
    'height': {'width': '100px', 'height': '100px'},
    'min-height': {'width': '100px', 'height': '100px'},
    'max-height': {'width': '100px', 'height': '100px'},
    'width': {'width': '100px', 'height': '100px'},
    'min-width': {'width': '100px', 'height': '100px'},
    'max-width': {'width': '100px', 'height': '100px'},
    // unspecified properties
    'position': {'position': 'relative', 'width': '100px', 'height': '100px'},
    // inheritance tests
    'top': {'width': '100px', 'height': '100px', 'position': 'relative'},
    'right': {'width': '100px', 'height': '100px', 'position': 'relative'},
    'bottom': {'width': '100px', 'height': '100px', 'position': 'relative'},
    'left': {'width': '100px', 'height': '100px', 'position': 'relative'}
};


function assemble(props) {
    var tests = [];

    // assemble tests
    for (var property in props) {
        props[property].forEach(function(type) {
            var _values = values[type](property);
            Object.keys(_values).forEach(function(unit) {
                var data = {
                    name: property + ' ' + type + '(' + unit + ')',
                    property: property,
                    valueType : type,
                    unit : unit,
                    parentStyle: extend({}, parent_styles[property] || {}),
                    from: extend({}, additional_styles[property] || {}),
                    to: {}
                };

                data.from[property] = _values[unit][0];
                data.to[property] = _values[unit][1];
                data.flags = _values[unit][2] || {};

                tests.push(data);
            });
        });
    }

    return tests;
}

root.getPropertyTests = function() {
    return assemble(properties);
};

root.getMissingPropertyTests = function() {
    return assemble(missing_properties);
};

root.getUnspecifiedPropertyTests = function() {
    return assemble(unspecified_properties);
};

root.getFontSizeRelativePropertyTests = function() {
    var accepted = {};

    for (var key in properties) {
        if (!Object.prototype.hasOwnProperty.call(properties, key) || key === "font-size") {
            continue;
        }

        if (properties[key].indexOf('length') > -1) {
            accepted[key] = ['length-em'];
        }
    }

    return assemble(accepted);
};

root.filterPropertyTests = function(tests, names) {
    var allowed = {};
    var accepted = [];

    if (typeof names === "string") {
        names = [names];
    }

    if (!(names instanceof RegExp)) {
        names.forEach(function(name) {
            allowed[name] = true;
        });
    }

    tests.forEach(function(test) {
        if (names instanceof RegExp) {
            if (!test.name.match(names)) {
                return;
            }
        } else if (!allowed[test.name]) {
            return;
        }

        accepted.push(test);
    });

    return accepted;
};

})(window);
