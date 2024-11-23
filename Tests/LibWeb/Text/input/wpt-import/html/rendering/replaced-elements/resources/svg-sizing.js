// Simple implementation of SVG sizing

setup({explicit_done: true});

var SVGSizing = (function() {
    function parseLength(l) {
        var match = /^([-+]?[0-9]+|[-+]?[0-9]*\.[0-9]+)(px|%)?$/.exec(l);
        if (!match)
            return null;
        return new Length(Number(match[1]), match[2] ? match[2] : "px");
    }

    function parseViewBox(input) {
        if (!input)
            return null;

        var arr = input.split(' ');
        return arr.map(function(a) { return parseInt(a); });
    }

    // Only px and % are used
    function convertToPx(input, percentRef) {
        if (input == null)
            return null;
        var length = parseLength(input);
        if (length.amount == 0)
            return 0;
        if (!length.unit)
            length.unit = "px";
        if (length.unit == "%" && percentRef === undefined)
            return null;
        return length.amount * { px: 1,
                                 "%": percentRef/100}[length.unit];
    }

    function Length(amount, unit) {
        this.amount = amount;
        this.unit = unit;
    }

    function describe(data) {
        function dumpObject(obj) {
            var r = "";
            for (var property in obj) {
                if (obj.hasOwnProperty(property)) {
                    var value = obj[property];
                    if (typeof value == 'string')
                        value = "'" + value + "'";
                    else if (value == null)
                        value = "null";
                    else if (typeof value == 'object')
                    {
                        if (value instanceof Array)
                            value = "[" + value + "]";
                        else
                            value = "{" + dumpObject(value) + "}";
                    }

                    if (value != "null")
                        r += property + ": " + value + ", ";
                }
            }
            return r;
        }
        var result = dumpObject(data);
        if (result == "")
            return "(initial values)";
        return result;
    }

    function mapPresentationalHintLength(testData, cssProperty, attr) {
        if (attr) {
            var l = parseLength(attr);
            if (l)
                testData.style[cssProperty] = l.amount + l.unit;
        }
    }

    function computedWidthIsAuto(testData) {
        return !testData.style["width"] || testData.style["width"] == 'auto';
    }

    function computedHeightIsAuto(testData) {
        return !testData.style["height"] || testData.style["height"] == 'auto' ||
            (parseLength(testData.style["height"]).unit == '%' &&
             containerComputedHeightIsAuto(testData));
    }

    function containerComputedWidthIsAuto(testData) {
        return !testData.config.containerWidthStyle ||
            testData.config.containerWidthStyle == 'auto';
    }

    function containerComputedHeightIsAuto(testData) {
        return !testData.config.containerHeightStyle ||
            testData.config.containerHeightStyle == 'auto';
    }

    function intrinsicInformation(testData) {
        if (testData.config.placeholder == 'iframe')
            return {};

        var w = convertToPx(testData.config.svgWidthAttr) || 0;
        var h = convertToPx(testData.config.svgHeightAttr) || 0;
        var r = 0;
        if (w && h) {
            r =  w / h;
        } else {
            var vb = parseViewBox(testData.config.svgViewBoxAttr);
            if (vb) {
                r = vb[2] / vb[3];
            }
            if (r) {
                if (!w && h)
                    w = h * r;
                else if (!h && w)
                    h = w / r;
            }
        }
        return { width: w, height: h, ratio: r };
    };

    function contentAttributeForPlaceholder(testData) {
        if (testData.config.placeholder == 'object')
            return "data";
        else
            return "src";
    }

    function TestData(config) {
        this.config = config;
        this.name = describe(config);
        this.style = {};
        if (config.placeholder) {
            mapPresentationalHintLength(this, "width", config.placeholderWidthAttr);
            mapPresentationalHintLength(this, "height", config.placeholderHeightAttr);
        } else {
            if (config.svgWidthStyle)
                this.style["width"] = config.svgWidthStyle;
            else
                mapPresentationalHintLength(this, "width", config.svgWidthAttr);

            if (config.svgHeightStyle)
                this.style["height"] = config.svgHeightStyle;
            else
                mapPresentationalHintLength(this, "height", config.svgHeightAttr);
        }
    }

    TestData.prototype.computeInlineReplacedSize = function(outerWidth, outerHeight) {
        var intrinsic = intrinsicInformation(this);
        var self = this;

        // http://www.w3.org/TR/CSS2/visudet.html#inline-replaced-height
        function calculateUsedHeight() {
            if (computedHeightIsAuto(self)) {
                if (computedWidthIsAuto(self) && intrinsic.height)
                    return intrinsic.height;
                if (intrinsic.ratio)
                    return calculateUsedWidth() / intrinsic.ratio;
                if (intrinsic.height)
                    return intrinsic.height;
                return 150;
            }

            return convertToPx(self.style["height"],
                               convertToPx(self.config.containerHeightStyle,
                                           outerHeight));
        }

        // http://www.w3.org/TR/CSS2/visudet.html#inline-replaced-width
        function calculateUsedWidth() {
            if (computedWidthIsAuto(self)) {
                if (computedHeightIsAuto(self) && intrinsic.width)
                    return intrinsic.width;
                if (!computedHeightIsAuto(self) && intrinsic.ratio)
                    return calculateUsedHeight() * intrinsic.ratio;
                if (computedHeightIsAuto(self) && intrinsic.ratio) {
                    if (containerComputedWidthIsAuto(self)) {
                        // Note: While this is actually undefined in CSS
                        // 2.1, use the suggested value by examining the
                        // ancestor widths.
                        return outerWidth;
                    } else {
                        return convertToPx(self.config.containerWidthStyle,
                                           outerWidth);
                    }
                }
                if (intrinsic.width)
                    return intrinsic.width;
                return 300;
            }

            if (containerComputedWidthIsAuto(self))
                return convertToPx(self.style["width"], outerWidth);
            else
                return convertToPx(self.style["width"],
                                   convertToPx(self.config.containerWidthStyle,
                                               outerWidth));
        }
        return { width: calculateUsedWidth(),
                 height: calculateUsedHeight() };
    };

    TestData.prototype.buildContainer = function (placeholder, options) {
        options = options || {};

        var container = document.createElement("div");

        container.id = "container";
        if (this.config.containerWidthStyle)
            container.style.width = this.config.containerWidthStyle;

        if (this.config.containerHeightStyle)
            container.style.height = this.config.containerHeightStyle;

        if (options.pretty)
            container.appendChild(document.createTextNode("\n\t\t"));
        container.appendChild(placeholder);
        if (options.pretty)
            container.appendChild(document.createTextNode("\n\t"));

        return container;
    };

    TestData.prototype.buildSVGOrPlaceholder = function (options) {
        options = options || {};
        var self = this;

        if (this.config.placeholder) {
            var generateSVGURI = function(testData, encoder) {
                var res = '<svg xmlns="http://www.w3.org/2000/svg"';
                function addAttr(attr, prop) {
                    if (testData.config[prop])
                        res += ' ' + attr + '="' + testData.config[prop] + '"';
                }
                addAttr("width", "svgWidthAttr");
                addAttr("height", "svgHeightAttr");
                addAttr("viewBox", "svgViewBoxAttr");
                res += '></svg>';
                return 'data:image/svg+xml' + encoder(res);
            };
            var placeholder = document.createElement(this.config.placeholder);
            if (options.pretty) {
                placeholder.appendChild(document.createTextNode("\n\t\t\t"));
                placeholder.appendChild(
                    document.createComment(
                        generateSVGURI(this, function(x) { return "," + x; })));
                placeholder.appendChild(document.createTextNode("\n\t\t"));
            }
            placeholder.setAttribute("id", "test");
            if (this.config.placeholderWidthAttr)
                placeholder.setAttribute("width", this.config.placeholderWidthAttr);
            if (this.config.placeholderHeightAttr)
                placeholder.setAttribute("height", this.config.placeholderHeightAttr);
            placeholder.setAttribute(contentAttributeForPlaceholder(this),
                                     generateSVGURI(this, function(x) {
                                         return ";base64," + btoa(x);
                                     }));
            return placeholder;
        } else {
            var svgElement = document.createElementNS("http://www.w3.org/2000/svg", "svg");
            svgElement.setAttribute("id", "test");
            if (self.config.svgWidthStyle)
                svgElement.style.width = self.config.svgWidthStyle;
            if (self.config.svgHeightStyle)
                svgElement.style.height = self.config.svgHeightStyle;
            if (self.config.svgWidthAttr)
                svgElement.setAttribute("width", self.config.svgWidthAttr);
            if (self.config.svgHeightAttr)
                svgElement.setAttribute("height", self.config.svgHeightAttr);
            if (self.config.svgViewBoxAttr)
                svgElement.setAttribute("viewBox", self.config.svgViewBoxAttr);
            return svgElement;
        }
    };

    TestData.prototype.buildDemo = function (expectedRect, id) {
        // Non-essential debugging tool
        var self = this;

        function buildDemoSerialization() {
            var outerWidth = 800;
            var outerHeight = 600;

            var options = { pretty: true };
            var container =
                    self.buildContainer(self.buildSVGOrPlaceholder(options), options);

            var root = document.createElement("html");
            var style = document.createElement("style");

            style.textContent = "\n" +
                "\tbody { margin: 0; font-family: sans-serif }\n" +
                "\tiframe { border: none }\n" +
                "\t#expected {\n" +
                "\t\twidth: " + (expectedRect.width) + "px; height: "
                + (expectedRect.height) + "px;\n" +
                "\t\tborder: 10px solid lime; position: absolute;\n" +
                "\t\tbackground-color: red }\n" +
                "\t#testContainer { position: absolute;\n" +
                "\t\ttop: 10px; left: 10px; width: " + outerWidth + "px;\n" +
                "\t\theight: " + outerHeight + "px }\n" +
                "\t#test { background-color: green }\n" +
                "\t.result { position: absolute; top: 0; right: 0;\n" +
                "\t\tbackground-color: hsla(0,0%, 0%, 0.85); border-radius: 0.5em;\n" +
                "\t\tpadding: 0.5em; border: 0.25em solid black }\n" +
                "\t.pass { color: lime }\n" +
                "\t.fail { color: red }\n";

            root.appendChild(document.createTextNode("\n"));
            root.appendChild(style);
            root.appendChild(document.createTextNode("\n"));

            var script = document.createElement("script");
            script.textContent = "\n" +
                "onload = function() {\n" +
                "\tvar svgRect =\n" +
                "\t\tdocument.querySelector('#test').getBoundingClientRect();\n" +
                "\tpassed = (svgRect.width == " + expectedRect.width + " && " +
                "svgRect.height == " + expectedRect.height + ");\n" +
                "\tdocument.body.insertAdjacentHTML('beforeEnd',\n" +
                "\t\t'<span class=\"result '+ (passed ? 'pass' : 'fail') " +
                "+ '\">' + (passed ? 'Pass' : 'Fail') + '</span>');\n" +
                "};\n";

            root.appendChild(script);
            root.appendChild(document.createTextNode("\n"));

            var expectedElement = document.createElement("div");
            expectedElement.id = "expected";
            root.appendChild(expectedElement);
            root.appendChild(document.createTextNode("\n"));

            var testContainer = document.createElement("div");
            testContainer.id = "testContainer";
            testContainer.appendChild(document.createTextNode("\n\t"));
            testContainer.appendChild(container);
            testContainer.appendChild(document.createTextNode("\n"));
            root.appendChild(testContainer);
            root.appendChild(document.createTextNode("\n"));

            return "<!DOCTYPE html>\n" + root.outerHTML;
        }

        function pad(n, width, z) {
            z = z || '0';
            n = n + '';
            return n.length >= width ? n : new Array(width - n.length + 1).join(z) + n;
        }

        function heightToDescription(height) {
            if (!height || height == "auto")
                return "auto";
            if (parseLength(height).unit == '%')
                return "percentage";
            return "fixed";
        }

        var demoRoot = document.querySelector('#demo');
        if (demoRoot) {
            var demo = buildDemoSerialization();
            var iframe = document.createElement('iframe');
            iframe.style.width = (Math.max(900, expectedRect.width)) + "px";
            iframe.style.height = (Math.max(400, expectedRect.height)) + "px";
            iframe.src = "data:text/html;charset=utf-8," + encodeURIComponent(demo);
            demoRoot.appendChild(iframe);
            demoRoot.insertAdjacentHTML(
                'beforeEnd',
                '<p><a href="data:application/octet-stream;charset=utf-8;base64,' +
                    btoa(demo) + '" download="svg-in-' + this.config.placeholder + "-" +
                    heightToDescription(this.config.placeholderHeightAttr) + "-" + pad(id, 3) +
                    '.html">Download</a></p>');
        }
    };

    return {
        TestData: TestData,
        doCombinationTest: function(values, func, testSingleId) {
            function computeConfig(id) {
                id--;
                var multiplier = 1;
                var config = {};
                for (var i=0; i<values.length; i++) {
                    // Compute offset into current array
                    var ii = (Math.floor(id / multiplier)) % values[i][1].length;
                    // Set corresponding value
                    config[values[i][0]] = values[i][1][ii];
                    // Compute new multiplier
                    multiplier *= values[i][1].length;
                }
                if (id >= multiplier)
                    return null;
                return config;
            }

            function cont(id) {
                var config = computeConfig(id);
                if (config && (!testSingleId || testSingleId == id)) {
                    var next = function() {func(config, id, cont)};
                    // Make sure we don't blow the stack, without too much slowness
                    if (id % 20 === 0) {
                        step_timeout(next, 0);
                    } else {
                        next();
                    }
                } else {
                    done();
                }
            };

            if (testSingleId)
                cont(testSingleId);
            else
                cont(1);
        }
    };
})();
