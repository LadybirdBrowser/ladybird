const PROPERTIES = {
  "alignment-baseline": {
    value: "middle",
    relevantElement: "text",
    irrelevantElement: "rect",
  },
  "baseline-shift": {
    value: "1",
    relevantElement: "text",
    irrelevantElement: "rect",
  },
  "clip-path": {
    value: "url(#e)",
    relevantElement: "g",
    irrelevantElement: "linearGradient",
  },
  "clip-rule": {
    value: "evenodd",
    relevantElement: "g",
    irrelevantElement: "linearGradient",
  },
  "color": {
    value: "blue",
    relevantElement: "g",
    irrelevantElement: "image",
  },
  "color-interpolation-filters": {
    value: "sRGB",
    relevantElement: "filter",
    irrelevantElement: "linearGradient",
  },
  "color-interpolation": {
    value: "linearRGB",
    relevantElement: "linearGradient",
    irrelevantElement: "image",
  },
  "cursor": {
    value: "pointer",
    relevantElement: "g",
    irrelevantElement: "defs",
  },
  "cx": {
    value: "1",
    relevantElement: "circle",
    irrelevantElement: null,
  },
  "cy": {
    value: "1",
    relevantElement: "circle",
    irrelevantElement: null,
  },
  "direction": {
    value: "rtl",
    relevantElement: "text",
    irrelevantElement: "rect",
  },
  "display": {
    value: "block",
    relevantElement: "g",
    irrelevantElement: "linearGradient",
  },
  "d": {
    value: "M0,0 L1,1",
    relevantElement: "path",
    irrelevantElement: null,
  },
  "dominant-baseline": {
    value: "middle",
    relevantElement: "text",
    irrelevantElement: "rect",
  },
  "fill": {
    value: "blue",
    relevantElement: "g",
    irrelevantElement: "image",
  },
  "fill-opacity": {
    value: "0.5",
    relevantElement: "g",
    irrelevantElement: "image",
  },
  "fill-rule": {
    value: "evenodd",
    relevantElement: "path",
    irrelevantElement: "image",
  },
  "filter": {
    value: "url(#e)",
    relevantElement: "g",
    irrelevantElement: "linearGradient",
  },
  "flood-color": {
    value: "blue",
    relevantElement: "feFlood",
    irrelevantElement: "rect",
  },
  "flood-opacity": {
    value: "0.5",
    relevantElement: "feFlood",
    irrelevantElement: "rect",
  },
  "font-family": {
    value: "Test Family",
    relevantElement: "text",
    irrelevantElement: "rect",
  },
  "font-size": {
    value: "50",
    relevantElement: "text",
    irrelevantElement: "rect",
  },
  "font-size-adjust": {
    value: "0.5",
    relevantElement: "text",
    irrelevantElement: "rect",
  },
  "font-stretch": {
    value: "expanded",
    relevantElement: "text",
    irrelevantElement: "rect",
  },
  "font-style": {
    value: "italic",
    relevantElement: "text",
    irrelevantElement: "rect",
  },
  "font-variant": {
    value: "small-caps",
    relevantElement: "text",
    irrelevantElement: "rect",
  },
  "font-weight": {
    value: "900",
    relevantElement: "text",
    irrelevantElement: "rect",
  },
  "glyph-orientation-vertical": {
    value: "90",
    relevantElement: "text",
    irrelevantElement: "rect",
  },
  "height": {
    value: "1",
    relevantElement: "rect",
    irrelevantElement: null,
  },
  "image-rendering": {
    value: ["optimizeSpeed", "pixelated"],
    relevantElement: "image",
    irrelevantElement: "path",
  },
  "letter-spacing": {
    value: "1px",
    relevantElement: "text",
    irrelevantElement: "rect",
  },
  "lighting-color": {
    value: "blue",
    relevantElement: "feDiffuseLighting",
    irrelevantElement: "rect",
  },
  "marker-end": {
    value: "url(#e)",
    relevantElement: "path",
    irrelevantElement: "image",
  },
  "marker-mid": {
    value: "url(#e)",
    relevantElement: "path",
    irrelevantElement: "image",
  },
  "marker-start": {
    value: "url(#e)",
    relevantElement: "path",
    irrelevantElement: "image",
  },
  "mask-type": {
    value: "alpha",
    relevantElement: "mask",
    irrelevantElement: "rect",
  },
  "mask": {
    value: "url(#e)",
    relevantElement: "g",
    irrelevantElement: "linearGradient",
  },
  "opacity": {
    value: "0.5",
    relevantElement: "g",
    irrelevantElement: "linearGradient",
  },
  "overflow": {
    value: "scroll",
    relevantElement: "svg",
    irrelevantElement: "rect",
  },
  "paint-order": {
    value: "fill stroke",
    relevantElement: "path",
    irrelevantElement: "image",
  },
  "pointer-events": {
    value: "none",
    relevantElement: "g",
    irrelevantElement: "linearGradient",
  },
  "r": {
    value: "1",
    relevantElement: "circle",
    irrelevantElement: null,
  },
  "rx": {
    value: "1",
    relevantElement: "rect",
    irrelevantElement: null,
  },
  "ry": {
    value: "1",
    relevantElement: "rect",
    irrelevantElement: null,
  },
  "shape-rendering": {
    value: "geometricPrecision",
    relevantElement: "path",
    irrelevantElement: "image",
  },
  "stop-color": {
    value: "blue",
    relevantElement: "stop",
    irrelevantElement: "rect",
  },
  "stop-opacity": {
    value: "0.5",
    relevantElement: "stop",
    irrelevantElement: "rect",
  },
  "stroke": {
    value: "blue",
    relevantElement: "path",
    irrelevantElement: "image",
  },
  "stroke-dasharray": {
    value: "1 1",
    relevantElement: "path",
    irrelevantElement: "image",
  },
  "stroke-dashoffset": {
    value: "1",
    relevantElement: "path",
    irrelevantElement: "image",
  },
  "stroke-linecap": {
    value: "round",
    relevantElement: "path",
    irrelevantElement: "image",
  },
  "stroke-linejoin": {
    value: "round",
    relevantElement: "path",
    irrelevantElement: "image",
  },
  "stroke-miterlimit": {
    value: "1",
    relevantElement: "path",
    irrelevantElement: "image",
  },
  "stroke-opacity": {
    value: "0.5",
    relevantElement: "path",
    irrelevantElement: "image",
  },
  "stroke-width": {
    value: "2",
    relevantElement: "path",
    irrelevantElement: "image",
  },
  "text-anchor": {
    value: "middle",
    relevantElement: "text",
    irrelevantElement: "rect",
  },
  "text-decoration": {
    value: "underline",
    relevantElement: "text",
    irrelevantElement: "rect",
  },
  "text-overflow": {
    value: "ellipsis",
    relevantElement: "text",
    irrelevantElement: "rect",
  },
  "text-rendering": {
    value: "geometricPrecision",
    relevantElement: "text",
    irrelevantElement: "rect",
  },
  "transform-origin": {
    value: "1px 1px",
    relevantElement: "g",
    irrelevantElement: "linearGradient",
  },
  "transform": {
    value: "scale(2)",
    relevantElement: "g",
    irrelevantElement: null,
  },
  "unicode-bidi": {
    value: "embed",
    relevantElement: "text",
    irrelevantElement: "rect",
  },
  "vector-effect": {
    value: "non-scaling-stroke",
    relevantElement: "g",
    irrelevantElement: "linearGradient",
  },
  "visibility": {
    value: "hidden",
    relevantElement: "g",
    irrelevantElement: "linearGradient",
  },
  "white-space": {
    value: "pre",
    relevantElement: "text",
    irrelevantElement: "rect",
  },
  "width": {
    value: "1",
    relevantElement: "rect",
    irrelevantElement: null,
  },
  "word-spacing": {
    value: "1",
    relevantElement: "text",
    irrelevantElement: "rect",
  },
  "writing-mode": {
    value: "vertical-rl",
    relevantElement: "text",
    irrelevantElement: "rect",
  },
  "x": {
    value: "1",
    relevantElement: "rect",
    irrelevantElement: null,
  },
  "y": {
    value: "1",
    relevantElement: "rect",
    irrelevantElement: null,
  },
};

function presentationAttributeIsSupported(element, attribute, value, property) {
  let e = document.createElementNS("http://www.w3.org/2000/svg", element);
  svg.append(e);
  let propertyValueBefore = getComputedStyle(e).getPropertyValue(property);
  e.setAttribute(attribute, value);
  // Also set another attribute that is likely to be a presentation attribute,
  // in order to provoke bugs.
  const otherAttribute = attribute === 'stroke' ? 'fill' : 'stroke';
  e.setAttribute(otherAttribute, 'red');
  let propertyValueAfter = getComputedStyle(e).getPropertyValue(property);
  e.remove();
  return propertyValueBefore != propertyValueAfter;
}

function assertPresentationAttributeIsSupported(element, attribute, values, property) {
  if (typeof values === 'string')
    values = [values];
  let supported = values.some(
    value => presentationAttributeIsSupported(element, attribute, value, property));
  assert_true(
    supported,
    `Presentation attribute ${attribute}="${values.join(" | ")}" should be supported on ${element} element`
  );
}

function assertPresentationAttributeIsNotSupported(element, attribute, values, property) {
  if (typeof values === 'string')
    values = [values];
  let supported = values.some(
    value => presentationAttributeIsSupported(element, attribute, value, property));
  assert_false(
    supported,
    `Presentation attribute ${attribute}="${values.join(" | ")}" should not be supported on ${element} element`
  );
}

function propertiesAreSupported(properties) {
  for (let p of properties) {
    if (!CSS.supports(p, "initial")) {
      return false;
    }
  }
  return true;
}
