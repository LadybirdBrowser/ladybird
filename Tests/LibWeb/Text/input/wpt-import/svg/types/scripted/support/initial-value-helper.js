const objects = {
  SVGCircleElement: 'circle',
  SVGClipPathElement: 'clipPath',
  SVGComponentTransferFunctionElement: 'feFuncA',
  SVGEllipseElement: 'ellipse',
  SVGFEBlendElement: 'feBlend',
  SVGFEColorMatrixElement: 'feColorMatrix',
  SVGFECompositeElement: 'feComposite',
  SVGFEConvolveMatrixElement: 'feConvolveMatrix',
  SVGFEDiffuseLightingElement: 'feDiffuseLighting',
  SVGFEDisplacementMapElement: 'feDisplacementMap',
  SVGFEDistantLightElement: 'feDistantLight',
  SVGFEDropShadowElement: 'feDropShadow',
  SVGFEGaussianBlurElement: 'feGaussianBlur',
  SVGFEMorphologyElement: 'feMorphology',
  SVGFEOffsetElement: 'feOffset',
  SVGFEPointLightElement: 'fePointLight',
  SVGFESpecularLightingElement: 'feSpecularLighting',
  SVGFESpotLightElement: 'feSpotLight',
  SVGFETurbulenceElement: 'feTurbulence',
  SVGFilterElement: 'filter',
  SVGFilterPrimitiveStandardAttributes: 'feBlend',
  SVGForeignObjectElement: 'foreignObject',
  SVGGeometryElement: 'rect',
  SVGGradientElement: 'linearGradient',
  SVGImageElement: 'image',
  SVGLineElement: 'line',
  SVGLinearGradientElement: 'linearGradient',
  SVGMarkerElement: 'marker',
  SVGMaskElement: 'mask',
  SVGPatternElement: 'pattern',
  SVGRadialGradientElement: 'radialGradient',
  SVGRectElement: 'rect',
  SVGSVGElement: 'svg',
  SVGStopElement: 'stop',
  SVGTextContentElement: 'text',
  SVGTextPathElement: 'textPath',
  SVGUseElement: 'use',
};

function assert_initial_values(attribute_map, config) {
  if (typeof config == 'undefined')
    config = {};
  let getValue = config.getValue || function(value) { return value; };
  for (let info of attribute_map) {
    for (let attribute of info.attributes) {
      let content_attribute = config.mapProperty && config.mapProperty[attribute] || attribute;
      test(function() {
        let e = document.createElementNS('http://www.w3.org/2000/svg', objects[info.interface]);
        let initial = info[attribute] && info[attribute].initial || config.initial;
        let valid = info[attribute] && info[attribute].valid || config.valid;
        assert_equals(getValue(e[attribute].baseVal), initial, 'initial before');
        e.setAttribute(content_attribute, valid);
        assert_not_equals(getValue(e[attribute].baseVal), initial, 'new value');
        e.removeAttribute(content_attribute);
        assert_equals(getValue(e[attribute].baseVal), initial, 'initial after');
      }, document.title + ', ' + info.interface + '.prototype.' + attribute + ' (remove)');

      test(function() {
        let e = document.createElementNS('http://www.w3.org/2000/svg', objects[info.interface]);
        let initial = info[attribute] && info[attribute].initial || config.initial;
        let valid = info[attribute] && info[attribute].valid || config.valid;
        assert_equals(getValue(e[attribute].baseVal), initial, 'initial before');
        e.setAttribute(content_attribute, valid);
        assert_not_equals(getValue(e[attribute].baseVal), initial, 'new value');
        e.setAttribute(content_attribute, 'foobar');
        assert_equals(getValue(e[attribute].baseVal), initial, 'initial after');
      }, document.title + ', ' + info.interface + '.prototype.' + attribute + ' (invalid value)');
    }
  }
}
