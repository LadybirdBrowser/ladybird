'use strict';

const gCSSProperties1 = {
  'alignment-baseline': {
    // https://drafts.csswg.org/css-inline/#propdef-alignment-baseline
    types: [
      { type: 'discrete', options: [ [ 'baseline', 'middle' ] ] }
    ]
  },
  'align-content': {
    // https://drafts.csswg.org/css-align/#propdef-align-content
    types: [
      { type: 'discrete' , options: [ [ 'flex-start', 'flex-end' ] ] }
    ]
  },
  'align-items': {
    // https://drafts.csswg.org/css-align/#propdef-align-items
    types: [
      { type: 'discrete', options: [ [ 'flex-start', 'flex-end' ] ] }
    ]
  },
  'align-self': {
    // https://drafts.csswg.org/css-align/#propdef-align-self
    types: [
      { type: 'discrete', options: [ [ 'flex-start', 'flex-end' ] ] }
    ]
  },
  'anchor-name': {
    // https://drafts.csswg.org/css-anchor-position-1/#name
    types: [
      { type: 'discrete', options: [ [ 'none', '--anchor' ] ] }
    ]
  },
  'anchor-scope': {
    // https://drafts.csswg.org/css-anchor-position-1/#anchor-scope
    types: [
      { type: 'discrete', options: [ [ 'none', 'all' ] ] }
    ]
  },
  'appearance': {
    // https://drafts.csswg.org/css-ui/#appearance-switching
    types: [
      { type: 'discrete' , options: [ [ 'auto', 'none' ] ] }
    ]
  },
  'backface-visibility': {
    // https://drafts.csswg.org/css-transforms/#propdef-backface-visibility
    types: [
      { type: 'discrete', options: [ [ 'visible', 'hidden' ] ] }
    ]
  },
  'background-attachment': {
    // https://drafts.csswg.org/css-backgrounds-3/#background-attachment
    types: [
      { type: 'discrete', options: [ [ 'fixed', 'local' ] ] }
    ]
  },
  'background-color': {
    // https://drafts.csswg.org/css-backgrounds-3/#background-color
    types: [ 'color' ]
  },
  'background-blend-mode': {
    // https://drafts.fxtf.org/compositing-1/#propdef-background-blend-mode
    types: [
      { type: 'discrete', options: [ [ 'multiply', 'screen' ] ] }
    ]
  },
  'background-clip': {
    // https://drafts.csswg.org/css-backgrounds-3/#background-clip
    types: [
      { type: 'discrete', options: [ [ 'padding-box', 'content-box' ] ] }
    ]
  },
  'background-image': {
    // https://drafts.csswg.org/css-backgrounds-3/#background-image
    types: [
      { type: 'discrete',
        options: [ [ 'url("http://localhost/test-1")',
                   'url("http://localhost/test-2")' ] ] }
    ]
  },
  'background-origin': {
    // https://drafts.csswg.org/css-backgrounds-3/#background-origin
    types: [
      { type: 'discrete', options: [ [ 'padding-box', 'content-box' ] ] }
    ]
  },
  'background-position': {
    // https://drafts.csswg.org/css-backgrounds-3/#background-position
    types: [
    ]
  },
  'background-position-x': {
    // https://drafts.csswg.org/css-backgrounds-4/#propdef-background-position-x
    types: [
    ]
  },
  'background-position-y': {
    // https://drafts.csswg.org/css-backgrounds-4/#propdef-background-position-y
    types: [
    ]
  },
  'background-repeat': {
    // https://drafts.csswg.org/css-backgrounds-3/#background-repeat
    types: [
      { type: 'discrete', options: [ [ 'space', 'round' ] ] }
    ]
  },
  'background-size': {
    // https://drafts.csswg.org/css-backgrounds-3/#background-size
    types: [
    ]
  },
  'block-size': {
    // https://drafts.csswg.org/css-logical-props/#propdef-block-size
    types: [
    ]
  },
  'block-step-align': {
    // https://drafts.csswg.org/css-rhythm/#block-step-align
    types: [
      { type: 'discrete', options: [ [ 'auto', 'center'], ['end', 'start'], ['start', 'center'] ] }
    ]
  },
  'block-step-insert': {
    // https://drafts.csswg.org/css-rhythm/#block-step-insert
    types: [
      { type: 'discrete', options: [ [ 'margin-box', 'padding-box'], ['margin-box', 'content-box'], ['padding-box', 'content-box'] ] }
    ]
  },
  'block-step-round': {
    // https://drafts.csswg.org/css-rhythm/#block-step-round
    types: [
      { type: 'discrete', options: [ [ 'up', 'down'], ['down', 'nearest'], ['nearest', 'up'] ] }
    ]
  },
  'block-step-size': {
    // https://drafts.csswg.org/css-rhythm/#block-step-size
    types: [ 'length' ]
  },
  'border-block-end-color': {
    // https://drafts.csswg.org/css-logical-props/#propdef-border-block-end-color
    types: [
    ]
  },
  'border-block-end-style': {
    // https://drafts.csswg.org/css-logical-props/#propdef-border-block-end-style
    types: [
    ]
  },
  'border-block-end-width': {
    // https://drafts.csswg.org/css-logical-props/#propdef-border-block-end-width
    types: [
    ]
  },
  'border-block-start-color': {
    // https://drafts.csswg.org/css-logical-props/#propdef-border-block-start-color
    types: [
    ]
  },
  'border-block-start-style': {
    // https://drafts.csswg.org/css-logical-props/#propdef-border-block-start-style
    types: [
    ]
  },
  'border-block-start-width': {
    // https://drafts.csswg.org/css-logical-props/#propdef-border-block-start-width
    types: [
    ]
  },
  'border-bottom-color': {
    // https://drafts.csswg.org/css-backgrounds-3/#border-bottom-color
    types: [ 'color' ]
  },
  'border-bottom-left-radius': {
    // https://drafts.csswg.org/css-backgrounds-3/#border-bottom-left-radius
    types: [
    ]
  },
  'border-bottom-right-radius': {
    // https://drafts.csswg.org/css-backgrounds-3/#border-bottom-right-radius
    types: [
    ]
  },
  'border-bottom-style': {
    // https://drafts.csswg.org/css-backgrounds-3/#border-bottom-style
    types: [
      { type: 'discrete', options: [ [ 'dotted', 'solid' ] ] }
    ]
  },
  'border-bottom-width': {
    // https://drafts.csswg.org/css-backgrounds-3/#border-bottom-width
    types: [ 'length' ],
    setup: t => {
      const element = createElement(t);
      element.style.borderBottomStyle = 'solid';
      return element;
    }
  },
  'border-collapse': {
    // https://drafts.csswg.org/css-tables/#propdef-border-collapse
    types: [
      { type: 'discrete', options: [ [ 'collapse', 'separate' ] ] }
    ]
  },
  'border-inline-end-color': {
    // https://drafts.csswg.org/css-logical-props/#propdef-border-inline-end-color
    types: [
    ]
  },
  'border-inline-end-style': {
    // https://drafts.csswg.org/css-logical-props/#propdef-border-inline-end-style
    types: [
    ]
  },
  'border-inline-end-width': {
    // https://drafts.csswg.org/css-logical-props/#propdef-border-inline-end-width
    types: [
    ]
  },
  'border-inline-start-color': {
    // https://drafts.csswg.org/css-logical-props/#propdef-border-inline-start-color
    types: [
    ]
  },
  'border-inline-start-style': {
    // https://drafts.csswg.org/css-logical-props/#propdef-border-block-start-style
    types: [
    ]
  },
  'border-inline-start-width': {
    // https://drafts.csswg.org/css-logical-props/#propdef-border-inline-start-width
    types: [
    ]
  },
  'border-image-outset': {
    // https://drafts.csswg.org/css-backgrounds-3/#border-image-outset
    types: [
    ]
  },
  'border-image-repeat': {
    // https://drafts.csswg.org/css-backgrounds-3/#border-image-repeat
    types: [
      { type: 'discrete', options: [ [ 'stretch repeat', 'round space' ] ] }
    ]
  },
  'border-image-slice': {
    // https://drafts.csswg.org/css-backgrounds-3/#border-image-slice
    types: [
    ]
  },
  'border-image-source': {
    // https://drafts.csswg.org/css-backgrounds-3/#border-image-source
    types: [
      { type: 'discrete',
        options: [ [ 'url("http://localhost/test-1")',
                   'url("http://localhost/test-2")' ] ] }
    ]
  },
  'border-image-width': {
    // https://drafts.csswg.org/css-backgrounds-3/#border-image-width
    types: [
    ]
  },
  'border-left-color': {
    // https://drafts.csswg.org/css-backgrounds-3/#border-left-color
    types: [ 'color' ]
  },
  'border-left-style': {
    // https://drafts.csswg.org/css-backgrounds-3/#border-left-style
    types: [
      { type: 'discrete', options: [ [ 'dotted', 'solid' ] ] }
    ]
  },
  'border-left-width': {
    // https://drafts.csswg.org/css-backgrounds-3/#border-left-width
    types: [ 'length' ],
    setup: t => {
      const element = createElement(t);
      element.style.borderLeftStyle = 'solid';
      return element;
    }
  },
  'border-right-color': {
    // https://drafts.csswg.org/css-backgrounds-3/#border-right-color
    types: [ 'color' ]
  },
  'border-right-style': {
    // https://drafts.csswg.org/css-backgrounds-3/#border-right-style
    types: [
      { type: 'discrete', options: [ [ 'dotted', 'solid' ] ] }
    ]
  },
  'border-right-width': {
    // https://drafts.csswg.org/css-backgrounds-3/#border-right-width
    types: [ 'length' ],
    setup: t => {
      const element = createElement(t);
      element.style.borderRightStyle = 'solid';
      return element;
    }
  },
  'border-spacing': {
    // https://drafts.csswg.org/css-tables/#propdef-border-spacing
    types: [ 'lengthPair' ]
  },
  'border-top-color': {
    // https://drafts.csswg.org/css-backgrounds-3/#border-top-color
    types: [ 'color' ]
  },
  'border-top-left-radius': {
    // https://drafts.csswg.org/css-backgrounds-3/#border-top-left-radius
    types: [
    ]
  },
  'border-top-right-radius': {
    // https://drafts.csswg.org/css-backgrounds-3/#border-top-right-radius
    types: [
    ]
  },
  'border-top-style': {
    // https://drafts.csswg.org/css-backgrounds-3/#border-top-style
    types: [
      { type: 'discrete', options: [ [ 'dotted', 'solid' ] ] }
    ]
  },
  'border-top-width': {
    // https://drafts.csswg.org/css-backgrounds-3/#border-top-width
    types: [ 'length' ],
    setup: t => {
      const element = createElement(t);
      element.style.borderTopStyle = 'solid';
      return element;
    }
  },
  'bottom': {
    // https://drafts.csswg.org/css-position/#propdef-bottom
    types: [
    ]
  },
  'box-decoration-break': {
    // https://drafts.csswg.org/css-break/#propdef-box-decoration-break
    types: [
      { type: 'discrete', options: [ [ 'slice', 'clone' ] ] }
    ]
  },
  'box-shadow': {
    // https://drafts.csswg.org/css-backgrounds/#box-shadow
    types: [ 'boxShadowList' ],
  },
  'box-sizing': {
    // https://drafts.csswg.org/css-ui-4/#box-sizing
    types: [
      { type: 'discrete', options: [ [ 'content-box', 'border-box' ] ] }
    ]
  },
  'buffered-rendering': {
    // https://www.w3.org/TR/SVGTiny12/painting.html#BufferedRenderingProperty
    types: [
      { type: 'discrete', options: [ [ 'auto', 'dynamic' ] ] }
    ]
  },
  'caption-side': {
    // https://drafts.csswg.org/css-tables/#propdef-caption-side
    types: [
      { type: 'discrete', options: [ [ 'top', 'bottom' ] ] }
    ]
  },
  'caret-color': {
    // https://drafts.csswg.org/css-ui/#propdef-caret-color
    types: [ 'color' ]
  },
  'clear': {
    // https://drafts.csswg.org/css-page-floats/#propdef-clear
    types: [
      { type: 'discrete', options: [ [ 'left', 'right' ] ] }
    ]
  },
  'clip': {
    // https://drafts.fxtf.org/css-masking-1/#propdef-clip
    types: [
      'rect',
      { type: 'discrete', options: [ [ 'rect(10px, 10px, 10px, 10px)',
                                       'auto' ],
                                     [ 'rect(10px, 10px, 10px, 10px)',
                                       'rect(10px, 10px, 10px, auto)'] ] }
    ]
  },
  'clip-path': {
    // https://drafts.fxtf.org/css-masking-1/#propdef-clip-path
    types: [
    ]
  },
  'clip-rule': {
    // https://drafts.fxtf.org/css-masking-1/#propdef-clip-rule
    types: [
      { type: 'discrete', options: [ [ 'evenodd', 'nonzero' ] ] }
    ]
  },
  'color': {
    // https://drafts.csswg.org/css-color/#propdef-color
    types: [ 'color' ]
  },
  'color-adjust': {
    // https://drafts.csswg.org/css-color-4/#color-adjust
    types: [
      { type: 'discrete', options: [ [ 'economy', 'exact' ] ] }
    ]
  },
  'color-interpolation': {
    // https://w3c.github.io/svgwg/svg2-draft/painting.html#ColorInterpolationProperty
    types: [
      { type: 'discrete', options: [ [ 'linearrgb', 'auto' ] ] }
    ]
  },
  'color-interpolation-filters': {
    // https://drafts.fxtf.org/filters-1/#propdef-color-interpolation-filters
    types: [
      { type: 'discrete', options: [ [ 'srgb', 'linearrgb' ] ] }
    ]
  },
  'columns': {
    // https://drafts.csswg.org/css-multicol/#propdef-columns
    types: [ 'positiveInteger', 'length' ]
  },
  'column-count': {
    // https://drafts.csswg.org/css-multicol/#propdef-column-count
    types: [ 'positiveInteger',
            { type: 'discrete', options: [ [ 'auto', '10' ] ] }
    ]
  },
  'column-gap': {
    // https://drafts.csswg.org/css-multicol/#propdef-column-gap
    types: [ 'length',
            {  type: 'discrete', options: [ [ 'normal', '200px' ] ] }
    ]
  },
  'column-rule-color': {
    // https://drafts.csswg.org/css-multicol/#propdef-column-rule-color
    types: [ 'color' ]
  },
  'column-fill': {
    // https://drafts.csswg.org/css-multicol/#propdef-column-fill
    types: [
      { type: 'discrete', options: [ [ 'auto', 'balance' ] ] }
    ]
  },
  'column-wrap': {
    // https://drafts.csswg.org/css-multicol-2/#cwr
    types: [
      { type: 'discrete', options: [ [ 'nowrap', 'wrap' ] ] }
    ]
  },
  'column-rule-style': {
    // https://drafts.csswg.org/css-multicol/#propdef-column-rule-style
    types: [
      { type: 'discrete', options: [ [ 'none', 'dotted' ] ] }
    ]
  },
  'column-rule-width': {
    // https://drafts.csswg.org/css-multicol/#propdef-column-rule-width
    types: [ 'length' ],
    setup: t => {
      const element = createElement(t);
      element.style.columnRuleStyle = 'solid';
      return element;
    }
  },
  'column-width': {
    // https://drafts.csswg.org/css-multicol/#propdef-column-width
    types: [ 'length',
      { type: 'discrete', options: [ [ 'auto', '1px' ] ] }
    ]
  },
  'column-height': {
    // https://drafts.csswg.org/css-multicol-2/#ch
    types: [ 'length',
      { type: 'discrete', options: [ [ 'auto', '1px' ] ] }
    ]
  },
  'counter-increment': {
    // https://drafts.csswg.org/css-lists-3/#propdef-counter-increment
    types: [
      { type: 'discrete', options: [ [ 'ident-1 1', 'ident-2 2' ] ] }
    ]
  },
  'counter-reset': {
    // https://drafts.csswg.org/css-lists-3/#propdef-counter-reset
    types: [
      { type: 'discrete', options: [ [ 'ident-1 1', 'ident-2 2' ] ] }
    ]
  },
  'cursor': {
    // https://drafts.csswg.org/css2/ui.html#propdef-cursor
    types: [
      { type: 'discrete', options: [ [ 'pointer', 'wait' ] ] }
    ]
  },
  'dominant-baseline': {
    // https://drafts.csswg.org/css-inline/#propdef-dominant-baseline
    types: [
      { type: 'discrete', options: [ [ 'ideographic', 'alphabetic' ] ] }
    ]
  },
  'empty-cells': {
    // https://drafts.csswg.org/css-tables/#propdef-empty-cells
    types: [
      { type: 'discrete', options: [ [ 'show', 'hide' ] ] }
    ]
  },
  'field-sizing': {
    // https://drafts.csswg.org/css-ui/#field-sizing
    types: [
      { type: 'discrete', options: [ [ 'fixed', 'content' ] ] }
    ]
  },
  'fill': {
    // https://w3c.github.io/svgwg/svg2-draft/painting.html#FillProperty
    types: [
    ]
  },
  'fill-opacity': {
    // https://w3c.github.io/svgwg/svg2-draft/painting.html#FillOpacityProperty
    types: [ 'opacity' ]
  },
  'fill-rule': {
    // https://w3c.github.io/svgwg/svg2-draft/painting.html#FillRuleProperty
    types: [
      { type: 'discrete', options: [ [ 'evenodd', 'nonzero' ] ] }
    ]
  },
  'filter': {
    // https://drafts.fxtf.org/filters/#propdef-filter
    types: [ 'filterList' ]
  },
  'flex-basis': {
    // https://drafts.csswg.org/css-flexbox/#propdef-flex-basis
    types: [
      'lengthPercentageOrCalc',
      { type: 'discrete', options: [ [ 'auto', '10px' ] ] }
    ]
  },
  'flex-direction': {
    // https://drafts.csswg.org/css-flexbox/#propdef-flex-direction
    types: [
      { type: 'discrete', options: [ [ 'row', 'row-reverse' ] ] }
    ]
  },
  'flex-flow': {
    // https://drafts.csswg.org/css-flexbox/#propdef-flex-flow
    types: [
      { type: 'discrete', options: [ [ 'row', 'row-reverse wrap' ] ] }
    ]
  },
  'flex-grow': {
    // https://drafts.csswg.org/css-flexbox/#flex-grow-property
    types: [ 'positiveNumber' ]
  },
  'flex-shrink': {
    // https://drafts.csswg.org/css-flexbox/#propdef-flex-shrink
    types: [ 'positiveNumber' ]
  },
  'flex-wrap': {
    // https://drafts.csswg.org/css-flexbox/#propdef-flex-wrap
    types: [
      { type: 'discrete', options: [ [ 'nowrap', 'wrap' ] ] }
    ]
  },
  'flood-color': {
    // https://drafts.fxtf.org/filters/#FloodColorProperty
    types: [ 'color' ]
  },
  'flood-opacity': {
    // https://drafts.fxtf.org/filters/#propdef-flood-opacity
    types: [ 'opacity' ]
  },
  'font-size': {
    // https://drafts.csswg.org/css-fonts-3/#propdef-font-size
    types: [
    ]
  },
  'font-size-adjust': {
    // https://drafts.csswg.org/css-fonts-3/#propdef-font-size-adjust
    types: [
    ]
  },
  'font-stretch': {
    // https://drafts.csswg.org/css-fonts-4/#propdef-font-stretch
    types: [ 'percentage' ]
  },
  'font-style': {
    // https://drafts.csswg.org/css-fonts/#propdef-font-style
    types: [
      { type: 'discrete', options: [ [ 'italic', 'oblique' ] ] }
    ]
  },
  'font-width': {
    // https://drafts.csswg.org/css-fonts-4/#propdef-font-width
    types: [ 'percentage' ]
  },
  'float': {
    // https://drafts.csswg.org/css-page-floats/#propdef-float
    types: [
      { type: 'discrete', options: [ [ 'left', 'right' ] ] }
    ]
  },
  'font-family': {
    // https://drafts.csswg.org/css-fonts-3/#propdef-font-family
    types: [
      { type: 'discrete', options: [ [ 'helvetica', 'verdana' ] ] }
    ]
  },
  'font-feature-settings': {
    // https://drafts.csswg.org/css-fonts/#descdef-font-feature-settings
    types: [
      { type: 'discrete', options: [ [ '"liga" 5', 'normal' ] ] }
    ]
  },
  'font-kerning': {
    // https://drafts.csswg.org/css-fonts-3/#propdef-font-kerning
    types: [
      { type: 'discrete', options: [ [ 'auto', 'normal' ] ] }
    ]
  },
  'font-language-override': {
    // https://drafts.csswg.org/css-fonts-3/#propdef-font-language-override
    types: [
      { type: 'discrete', options: [ [ '"eng"', 'normal' ] ] }
    ]
  },
  'font-style': {
    // https://drafts.csswg.org/css-fonts-3/#propdef-font-style
    types: [
      { type: 'discrete', options: [ [ 'italic', 'oblique' ] ] }
    ]
  },
  'font-synthesis': {
    // https://drafts.csswg.org/css-fonts-3/#propdef-font-synthesis
    types: [
      { type: 'discrete', options: [ [ 'none', 'weight style' ] ] }
    ]
  },
  'font-variant-alternates': {
    // https://drafts.csswg.org/css-fonts-3/#propdef-font-variant-alternates
    types: [
      { type: 'discrete',
        options: [ [ 'swash(unknown)', 'stylistic(unknown)' ] ] }
    ]
  },
  'font-variant-caps': {
    // https://drafts.csswg.org/css-fonts-3/#propdef-font-variant-caps
    types: [
      { type: 'discrete', options: [ [ 'small-caps', 'unicase' ] ] }
    ]
  },
  'font-variant-east-asian': {
    // https://drafts.csswg.org/css-fonts-3/#propdef-font-variant-east-asian
    types: [
      { type: 'discrete', options: [ [ 'full-width', 'proportional-width' ] ] }
    ]
  },
  'font-variant-emoji': {
    // https://drafts.csswg.org/css-fonts/#propdef-font-variant-emoji
    types: [
      { type: 'discrete', options: [ [ 'text', 'emoji' ] ] }
    ]
  },
  'font-variant-ligatures': {
    // https://drafts.csswg.org/css-fonts-3/#propdef-font-variant-ligatures
    types: [
      { type: 'discrete',
        options: [ [ 'common-ligatures', 'no-common-ligatures' ] ] }
    ]
  },
  'font-variant-numeric': {
    // https://drafts.csswg.org/css-fonts-3/#propdef-font-variant-numeric
    types: [
      { type: 'discrete', options: [ [ 'lining-nums', 'oldstyle-nums' ] ] }
    ]
  },
  'font-variant-position': {
    // https://drafts.csswg.org/css-fonts-3/#propdef-font-variant-position
    types: [
      { type: 'discrete', options: [ [ 'sub', 'super' ] ] }
    ]
  },
  'font-variation-settings': {
    // https://drafts.csswg.org/css-fonts-4/#descdef-font-face-font-variation-settings
    types: [
      'fontVariationSettings',
      { type: 'discrete',
        options: [ ['"wdth" 1, "wght" 1.1', '"wdth" 5'],
                   ['"wdth" 5', 'normal']
                 ] },
    ]
  },
  'font-weight': {
    // https://drafts.csswg.org/css-fonts-3/#propdef-font-weight
    types: [
    ]
  },
  'gap': {
    // https://drafts.csswg.org/css-align/#propdef-gap
    types: [ 'length',
            {  type: 'discrete', options: [ [ 'normal', '200px' ] ] }
    ]
  },
  'grid-area': {
    // https://drafts.csswg.org/css-grid/#propdef-grid-area
    types: [
      { type: 'discrete', options: [ [ '1', '5' ] ] }
    ]
  },
  'grid-auto-columns': {
    // https://drafts.csswg.org/css-grid/#propdef-grid-auto-columns
    types: [
      { type: 'discrete', options: [ [ '1px', '5px' ] ] }
    ]
  },
  'grid-auto-flow': {
    // https://drafts.csswg.org/css-grid/#propdef-grid-auto-flow
    types: [
      { type: 'discrete', options: [ [ 'row', 'column' ] ] }
    ]
  },
  'grid-auto-rows': {
    // https://drafts.csswg.org/css-grid/#propdef-grid-auto-rows
    types: [
      { type: 'discrete', options: [ [ '1px', '5px' ] ] }
    ]
  },
  'grid-column': {
    // https://drafts.csswg.org/css-grid/#propdef-grid-column
    types: [
      { type: 'discrete', options: [ [ '1', '5' ] ] }
    ]
  },
  'grid-column-end': {
    // https://drafts.csswg.org/css-grid/#propdef-grid-column-end
    types: [
      { type: 'discrete', options: [ [ '1', '5' ] ] }
    ]
  },
  'grid-column-gap': {
    // https://drafts.csswg.org/css-grid/#propdef-grid-column-gap
    types: [
    ]
  },
  'grid-column-start': {
    // https://drafts.csswg.org/css-grid/#propdef-grid-column-start
    types: [
      { type: 'discrete', options: [ [ '1', '5' ] ] }
    ]
  },
  'grid-row': {
    // https://drafts.csswg.org/css-grid/#propdef-grid-row
    types: [
      { type: 'discrete', options: [ [ '1', '5' ] ] }
    ]
  },
  'grid-row-end': {
    // https://drafts.csswg.org/css-grid/#propdef-grid-row-end
    types: [
      { type: 'discrete', options: [ [ '1', '5' ] ] }
    ]
  },
  'grid-row-gap': {
    // https://drafts.csswg.org/css-grid/#propdef-grid-row-gap
    types: [
    ]
  },
  'grid-row-start': {
    // https://drafts.csswg.org/css-grid/#propdef-grid-row-start
    types: [
      { type: 'discrete', options: [ [ '1', '5' ] ] }
    ]
  },
  'grid-template': {
    // https://drafts.csswg.org/css-grid/#propdef-grid-template
    types: [
      { type: 'discrete', options: [ [ '". . a b" ". . a b"', 'none' ] ] }
    ]
  },
  'grid-template-areas': {
    // https://drafts.csswg.org/css-grid/#propdef-grid-template-areas
    types: [
      { type: 'discrete', options: [ [ '". . a b" ". . a b"', 'none' ] ] }
    ]
  },
  'height': {
    // https://drafts.csswg.org/css21/visudet.html#propdef-height
    types: [
    ]
  },
  'hanging-punctuation': {
    // https://drafts.csswg.org/css-text/#hanging-punctuation-property
    types: [
      { type: 'discrete', options: [ [ 'none', 'first last'] ] }
    ]
  },
  'hyphens': {
    // https://drafts.csswg.org/css-text-3/#propdef-hyphens
    types: [
      { type: 'discrete', options: [ [ 'manual', 'none' ] ] }
    ]
  },
  'image-orientation': {
    // https://drafts.csswg.org/css-images-3/#propdef-image-orientation
    types: [
      { type: 'discrete', options: [ [ 'none', 'from-image' ] ] }
    ]
  },
  'image-rendering': {
    // https://drafts.csswg.org/css-images-3/#propdef-image-rendering
    types: [
    ]
  },
  'ime-mode': {
    // https://drafts.csswg.org/css-ui/#input-method-editor
    types: [
      { type: 'discrete', options: [ [ 'disabled', 'auto' ] ] }
    ]
  },
  'initial-letter': {
    // https://drafts.csswg.org/css-inline/#propdef-initial-letter
    types: [
      { type: 'discrete', options: [ [ '1 2', '3 4' ] ] }
    ]
  },
};

const gCSSProperties2 = {
  'inline-size': {
    // https://drafts.csswg.org/css-logical-props/#propdef-inline-size
    types: [
    ]
  },
  'isolation': {
    // https://drafts.fxtf.org/compositing-1/#propdef-isolation
    types: [
      { type: 'discrete', options: [ [ 'auto', 'isolate' ] ] }
    ]
  },
  'justify-content': {
    // https://drafts.csswg.org/css-align/#propdef-justify-content
    types: [
      { type: 'discrete', options: [ [ 'start', 'end' ] ] }
    ]
  },
  'justify-items': {
    // https://drafts.csswg.org/css-align/#propdef-justify-items
    types: [
      { type: 'discrete', options: [ [ 'start', 'end' ] ] }
    ]
  },
  'justify-self': {
    // https://drafts.csswg.org/css-align/#propdef-justify-self
    types: [
      { type: 'discrete', options: [ [ 'start', 'end' ] ] }
    ]
  },
  'left': {
    // https://drafts.csswg.org/css-position/#propdef-left
    types: [
    ]
  },
  'letter-spacing': {
    // https://drafts.csswg.org/css-text-3/#propdef-letter-spacing
    types: [ 'lengthPercentageOrCalc' ]
  },
  'lighting-color': {
    // https://drafts.fxtf.org/filters/#LightingColorProperty
    types: [ 'color' ]
  },
  'line-height': {
    // https://drafts.csswg.org/css-inline/#line-height-property
    types: [
        { type: 'discrete', options: [ [ 'normal', '10px' ],
                                       [ 'normal', '10', 'normal', '100px' ] ] }
    ]
  },
  'list-style': {
    // https://drafts.csswg.org/css-lists-3/#propdef-list-style
    types: [
      { type: 'discrete', options: [ [ 'inside url("http://localhost/test-1") circle', 'outside url("http://localhost/test-2") square' ] ] },
    ]
  },
  'list-style-image': {
    // https://drafts.csswg.org/css-lists-3/#propdef-list-style-image
    types: [
      { type: 'discrete',
        options: [ [ 'url("http://localhost/test-1")',
                     'url("http://localhost/test-2")' ] ] }
    ]
  },
  'list-style-position': {
    // https://drafts.csswg.org/css-lists-3/#propdef-list-style-position
    types: [
      { type: 'discrete', options: [ [ 'inside', 'outside' ] ] }
    ]
  },
  'list-style-type': {
    // https://drafts.csswg.org/css-lists-3/#propdef-list-style-type
    types: [
      { type: 'discrete', options: [ [ 'circle', 'square' ] ] }
    ]
  },
  "math-depth": {
    // https://w3c.github.io/mathml-core/#propdef-math-depth
    types: [ "integer" ],
  },
  "math-shift": {
    // https://w3c.github.io/mathml-core/#propdef-math-shift
    types: [
      { type: "discrete", options: [ [ "normal", "compact" ] ] },
    ],
  },
  "math-style": {
    // https://w3c.github.io/mathml-core/#propdef-math-style
    types: [
      { type: "discrete", options: [ [ "normal", "compact" ] ] },
    ],
  },
  'margin-block-end': {
    // https://drafts.csswg.org/css-logical-props/#propdef-margin-block-end
    types: [
    ]
  },
  'margin-block-start': {
    // https://drafts.csswg.org/css-logical-props/#propdef-margin-block-start
    types: [
    ]
  },
  'margin-bottom': {
    // https://drafts.csswg.org/css-box/#propdef-margin-bottom
    types: [
    ]
  },
  'margin-inline-end': {
    // https://drafts.csswg.org/css-logical-props/#propdef-margin-inline-end
    types: [
    ]
  },
  'margin-inline-start': {
    // https://drafts.csswg.org/css-logical-props/#propdef-margin-inline-start
    types: [
    ]
  },
  'margin-left': {
    // https://drafts.csswg.org/css-box/#propdef-margin-left
    types: [
    ]
  },
  'margin-right': {
    // https://drafts.csswg.org/css-box/#propdef-margin-right
    types: [
    ]
  },
  'margin-top': {
    // https://drafts.csswg.org/css-box/#propdef-margin-top
    types: [
    ]
  },
  'marker-end': {
    // https://w3c.github.io/svgwg/specs/markers/#MarkerEndProperty
    types: [
      { type: 'discrete',
        options: [ [ 'url("http://localhost/test-1")',
                     'url("http://localhost/test-2")' ] ] }
    ]
  },
  'marker-mid': {
    // https://w3c.github.io/svgwg/specs/markers/#MarkerMidProperty
    types: [
      { type: 'discrete',
        options: [ [ 'url("http://localhost/test-1")',
                     'url("http://localhost/test-2")' ] ] }
    ]
  },
  'marker-start': {
    // https://w3c.github.io/svgwg/specs/markers/#MarkerStartProperty
    types: [
      { type: 'discrete',
        options: [ [ 'url("http://localhost/test-1")',
                     'url("http://localhost/test-2")' ] ] }
    ]
  },
  'mask': {
    // https://drafts.fxtf.org/css-masking-1/#the-mask
    types: [
      { type: 'discrete',
        options: [ [ 'url("http://localhost/test-1")',
                     'url("http://localhost/test-2")' ] ] }
    ]
  },
  'mask-clip': {
    // https://drafts.fxtf.org/css-masking-1/#propdef-mask-clip
    types: [
      { type: 'discrete', options: [ [ 'content-box', 'border-box' ] ] }
    ]
  },
  'mask-composite': {
    // https://drafts.fxtf.org/css-masking-1/#propdef-mask-composite
    types: [
      { type: 'discrete', options: [ [ 'add', 'subtract' ] ] }
    ]
  },
  'mask-image': {
    // https://drafts.fxtf.org/css-masking-1/#propdef-mask-image
    types: [
      { type: 'discrete',
        options: [ [ 'url("http://localhost/test-1")',
                     'url("http://localhost/test-2")' ] ] }
    ]
  },
  'mask-mode': {
    // https://drafts.fxtf.org/css-masking-1/#propdef-mask-mode
    types: [
      { type: 'discrete', options: [ [ 'alpha', 'luminance' ] ] }
    ]
  },
  'mask-origin': {
    // https://drafts.fxtf.org/css-masking-1/#propdef-mask-origin
    types: [
      { type: 'discrete', options: [ [ 'content-box', 'border-box' ] ] }
    ]
  },
  'mask-position': {
    // https://drafts.fxtf.org/css-masking-1/#propdef-mask-position
    types: [
    ]
  },
  'mask-position-x': {
    // https://lists.w3.org/Archives/Public/www-style/2014Jun/0166.html
    types: [
    ]
  },
  'mask-position-y': {
    // https://lists.w3.org/Archives/Public/www-style/2014Jun/0166.html
    types: [
    ]
  },
  'mask-repeat': {
    // https://drafts.fxtf.org/css-masking-1/#propdef-mask-repeat
    types: [
      { type: 'discrete', options: [ [ 'space', 'round' ] ] }
    ]
  },
  'mask-size': {
    // https://drafts.fxtf.org/css-masking-1/#propdef-mask-size
    types: [
    ]
  },
  'mask-type': {
    // https://drafts.fxtf.org/css-masking-1/#propdef-mask-type
    types: [
      { type: 'discrete', options: [ [ 'alpha', 'luminance' ] ] }
    ]
  },
  'max-block-size': {
    // https://drafts.csswg.org/css-logical-props/#propdef-max-block-size
    types: [
    ]
  },
  'max-height': {
    // https://drafts.csswg.org/css21/visudet.html#propdef-max-height
    types: [
    ]
  },
  'max-inline-size': {
    // https://drafts.csswg.org/css-logical-props/#propdef-max-inline-size
    types: [
    ]
  },
  'max-width': {
    // https://drafts.csswg.org/css21/visudet.html#propdef-max-width
    types: [
    ]
  },
  'min-block-size': {
    // https://drafts.csswg.org/css-logical-props/#propdef-min-block-size
    types: [
    ]
  },
  'min-height': {
    // https://drafts.csswg.org/css21/visudet.html#propdef-min-height
    types: [
    ]
  },
  'min-inline-size': {
    // https://drafts.csswg.org/css-logical-props/#propdef-min-inline-size
    types: [
    ]
  },
  'min-width': {
    // https://drafts.csswg.org/css21/visudet.html#propdef-min-width
    types: [
    ]
  },
  'mix-blend-mode': {
    // https://drafts.fxtf.org/compositing-1/#propdef-mix-blend-mode
    types: [
      { type: 'discrete', options: [ [ 'multiply', 'screen' ] ] }
    ]
  },
  'object-fit': {
    // https://drafts.csswg.org/css-images-3/#propdef-object-fit
    types: [
      { type: 'discrete', options: [ [ 'fill', 'contain' ] ] }
    ]
  },
  'object-position': {
    // https://drafts.csswg.org/css-images-3/#propdef-object-position
    types: [
    ]
  },
  'inset-block-end': {
    // https://drafts.csswg.org/css-logical-props/#propdef-inset-block-end
    types: [
    ]
  },
  'inset-block-start': {
    // https://drafts.csswg.org/css-logical-props/#propdef-inset-block-start
    types: [
    ]
  },
  'inset-inline-end': {
    // https://drafts.csswg.org/css-logical-props/#propdef-inset-inline-end
    types: [
    ]
  },
  'inset-inline-start': {
    // https://drafts.csswg.org/css-logical-props/#propdef-inset-inline-start
    types: [
    ]
  },
  'offset-distance': {
    // https://drafts.fxtf.org/motion-1/#offset-distance-property
    types: [ 'lengthPercentageOrCalc' ]
  },
  'offset-path': {
    // https://drafts.fxtf.org/motion-1/#offset-path-property
    types: [
    ]
  },
  'opacity': {
    // https://drafts.csswg.org/css-color/#propdef-opacity
    types: [
    ]
  },
  'order': {
    // https://drafts.csswg.org/css-flexbox/#propdef-order
    types: [ 'integer' ]
  },
  'outline-color': {
    // https://drafts.csswg.org/css-ui-3/#propdef-outline-color
    types: [ 'color' ]
  },
  'outline-offset': {
    // https://drafts.csswg.org/css-ui-3/#propdef-outline-offset
    types: [ 'length' ]
  },
  'outline-style': {
    // https://drafts.csswg.org/css-ui/#propdef-outline-style
    types: [
      { type: 'discrete', options: [ [ 'none', 'dotted' ] ] }
    ]
  },
  'outline-width': {
    // https://drafts.csswg.org/css-ui-3/#propdef-outline-width
    types: [ 'length' ],
    setup: t => {
      const element = createElement(t);
      element.style.outlineStyle = 'solid';
      return element;
    }
  },
  'overflow': {
    // https://drafts.csswg.org/css-overflow/#propdef-overflow
    types: [
      { type: 'discrete', options: [ [ 'visible', 'hidden' ] ] }
    ]
  },
  'overflow-wrap': {
    // https://drafts.csswg.org/css-text-3/#propdef-overflow-wrap
    types: [
      { type: 'discrete', options: [ [ 'normal', 'break-word' ] ] }
    ]
  },
  'overflow-x': {
    // https://drafts.csswg.org/css-overflow-3/#propdef-overflow-x
    types: [
      { type: 'discrete', options: [ [ 'visible', 'hidden' ] ] }
    ]
  },
  'overflow-y': {
    // https://drafts.csswg.org/css-overflow-3/#propdef-overflow-y
    types: [
      { type: 'discrete', options: [ [ 'visible', 'hidden' ] ] }
    ]
  },
  'padding-block-end': {
    // https://drafts.csswg.org/css-logical-props/#propdef-padding-block-end
    types: [
    ]
  },
  'padding-block-start': {
    // https://drafts.csswg.org/css-logical-props/#propdef-padding-block-start
    types: [
    ]
  },
  'padding-bottom': {
    // https://drafts.csswg.org/css-box/#propdef-padding-bottom
    types: [
    ]
  },
  'padding-inline-end': {
    // https://drafts.csswg.org/css-logical-props/#propdef-padding-inline-end
    types: [
    ]
  },
  'padding-inline-start': {
    // https://drafts.csswg.org/css-logical-props/#propdef-padding-inline-start
    types: [
    ]
  },
  'padding-left': {
    // https://drafts.csswg.org/css-box/#propdef-padding-left
    types: [
    ]
  },
  'padding-right': {
    // https://drafts.csswg.org/css-box/#propdef-padding-right
    types: [
    ]
  },
  'padding-top': {
    // https://drafts.csswg.org/css-box/#propdef-padding-top
    types: [
    ]
  },
  'page-break-after': {
    // https://drafts.csswg.org/css-break-3/#propdef-break-after
    types: [
      { type: 'discrete', options: [ [ 'always', 'auto' ] ] }
    ]
  },
  'page-break-before': {
    // https://drafts.csswg.org/css-break-3/#propdef-break-before
    types: [
      { type: 'discrete', options: [ [ 'always', 'auto' ] ] }
    ]
  },
  'page-break-inside': {
    // https://drafts.csswg.org/css-break-3/#propdef-break-inside
    types: [
      { type: 'discrete', options: [ [ 'auto', 'avoid' ] ] }
    ]
  },
  'paint-order': {
    // https://w3c.github.io/svgwg/svg2-draft/painting.html#PaintOrderProperty
    types: [
      { type: 'discrete', options: [ [ 'fill', 'stroke' ] ] }
    ]
  },
  'path-length': {
    // https://w3c.github.io/svgwg/svg2-draft/paths.html#PathLengthAttribute
    types: [ 'positiveNumber' ],
    setup: t => {
      return createElement(t, 'path');
    }
  },
  'perspective': {
    // https://drafts.csswg.org/css-transforms-1/#propdef-perspective
    types: [ 'length' ]
  },
  'perspective-origin': {
    // https://drafts.csswg.org/css-transforms-1/#propdef-perspective-origin
    types: [ 'position' ]
  },
  'place-content': {
    // https://drafts.csswg.org/css-align/#propdef-place-content
    types: [
      { type: 'discrete', options: [ [ 'start', 'end' ] ] }
    ]
  },
  'place-items': {
    // https://drafts.csswg.org/css-align/#propdef-place-items
    types: [
      { type: 'discrete', options: [ [ 'start', 'end' ] ] }
    ]
  },
  'place-self': {
    // https://drafts.csswg.org/css-align/#propdef-place-self
    types: [
      { type: 'discrete', options: [ [ 'start', 'end' ] ] }
    ]
  },
  'pointer-events': {
    // https://w3c.github.io/svgwg/svg2-draft/interact.html#PointerEventsProperty
    types: [
      { type: 'discrete', options: [ [ 'fill', 'none' ] ] }
    ]
  },
  'position': {
    // https://drafts.csswg.org/css-position/#propdef-position
    types: [
      { type: 'discrete', options: [ [ 'absolute', 'fixed' ] ] }
    ]
  },
  'position-anchor': {
    // https://drafts.csswg.org/css-anchor-position-1/#position-anchor
    types: [
      { type: 'discrete', options: [ [ 'auto', '--anchor' ] ] }
    ]
  },
  'position-visibility': {
    // https://drafts.csswg.org/css-anchor-position-1/#position-visibility
    types: [
      { type: 'discrete', options: [ [ 'always', 'anchors-valid' ] ] }
    ]
  },
  'position-try-fallbacks': {
    // https://drafts.csswg.org/css-anchor-position-1/#position-try-fallbacks
    types: [
      { type: 'discrete', options: [ [ 'none', 'flip-block' ] ] }
    ]
  },
  'position-try-order': {
    // https://drafts.csswg.org/css-anchor-position-1/#position-try-order-property
    types: [
      { type: 'discrete', options: [ [ 'normal', 'most-width' ] ] }
    ]
  },
  'quotes': {
    // https://drafts.csswg.org/css-content-3/#propdef-quotes
    types: [
      { type: 'discrete', options: [ [ '"“" "”" "‘" "’"', '"‘" "’" "“" "”"' ] ] }
    ]
  },
  'resize': {
    // https://drafts.csswg.org/css-ui/#propdef-resize
    types: [
      { type: 'discrete', options: [ [ 'both', 'horizontal' ] ] }
    ]
  },
  'right': {
    // https://drafts.csswg.org/css-position/#propdef-right
    types: [
    ]
  },
  'ruby-align': {
    // https://drafts.csswg.org/css-ruby-1/#propdef-ruby-align
    types: [
      { type: 'discrete', options: [ [ 'start', 'center' ] ] }
    ]
  },
  'ruby-position': {
    // https://drafts.csswg.org/css-ruby-1/#propdef-ruby-position
    types: [
      { type: 'discrete', options: [ [ 'under', 'over' ] ] }
    ],
    setup: t => {
      return createElement(t, 'ruby');
    }
  },
  'scroll-behavior': {
    // https://drafts.csswg.org/cssom-view/#propdef-scroll-behavior
    types: [
      { type: 'discrete', options: [ [ 'auto', 'smooth' ] ] }
    ]
  },
  'scroll-snap-align': {
    // https://drafts.csswg.org/css-scroll-snap/#propdef-scroll-snap-align
    types: [
      { type: 'discrete', options: [ [ 'none', 'start' ]] }
    ]
  },
  'scroll-snap-stop': {
    // https://drafts.csswg.org/css-scroll-snap/#propdef-scroll-snap-stop
    types: [
      { type: 'discrete', options: [ [ 'normal', 'always' ]] }
    ]
  },
  'scroll-snap-type': {
    // https://drafts.csswg.org/css-scroll-snap/#propdef-scroll-snap-type
    types: [
      { type: 'discrete', options: [ [ 'none', 'x mandatory' ]] }
    ]
  },
  'scrollbar-color': {
    // https://drafts.csswg.org/css-scrollbars/#propdef-scrollbar-color
    types: [ 'colorPair' ]
  },
  'scrollbar-gutter': {
    // https://drafts.csswg.org/css-overflow/#propdef-scrollbar-gutter
    types: [
      { type: 'discrete', options: [ [ 'auto', 'stable' ], [ 'auto', 'stable both-edges' ], [ 'stable', 'stable both-edges' ] ] }
    ]
  },
  'scrollbar-width': {
    // https://drafts.csswg.org/css-scrollbars/#propdef-scrollbar-width
    types: [
      { type: 'discrete', options: [ [ 'auto', 'thin' ], [ 'auto', 'none' ], [ 'thin', 'none' ] ] }
    ]
  },
  'shape-outside': {
    // http://dev.w3.org/csswg/css-shapes/#propdef-shape-outside
    types: [
      { type: 'discrete',
        options: [ [ 'url("http://localhost/test-1")',
                     'url("http://localhost/test-2")' ] ] }
    ]
  },
  'shape-rendering': {
    // https://w3c.github.io/svgwg/svg2-draft/painting.html#ShapeRenderingProperty
    types: [
      { type: 'discrete', options: [ [ 'optimizeSpeed', 'crispEdges' ] ] }
    ]
  },
  'stop-color': {
    // https://w3c.github.io/svgwg/svg2-draft/pservers.html#StopColorProperty
    types: [ 'color' ]
  },
  'stop-opacity': {
    // https://w3c.github.io/svgwg/svg2-draft/pservers.html#StopOpacityProperty
    types: [ 'opacity' ]
  },
  'stroke': {
    // https://w3c.github.io/svgwg/svg2-draft/painting.html#StrokeProperty
    types: [
    ]
  },
  'stroke-color': {
    // https://drafts.fxtf.org/fill-stroke-3/#propdef-stroke-color
    types: [ 'color' ]
  },
  'stroke-dasharray': {
    // https://w3c.github.io/svgwg/svg2-draft/painting.html#StrokeDasharrayProperty
    types: [
      'dasharray',
      { type: 'discrete', options: [ [ 'none', '10px, 20px' ] ] }
    ]
  },
  'stroke-dashoffset': {
    // https://w3c.github.io/svgwg/svg2-draft/painting.html#StrokeDashoffsetProperty
    types: [
    ]
  },
  'stroke-linecap': {
    // https://w3c.github.io/svgwg/svg2-draft/painting.html#StrokeLinecapProperty
    types: [
      { type: 'discrete', options: [ [ 'round', 'square' ] ] }
    ]
  },
  'stroke-linejoin': {
    // https://w3c.github.io/svgwg/svg2-draft/painting.html#StrokeLinejoinProperty
    types: [
      { type: 'discrete', options: [ [ 'round', 'miter' ] ] }
    ],
    setup: t => {
      return createElement(t, 'rect');
    }
  },
  'stroke-miterlimit': {
    // https://w3c.github.io/svgwg/svg2-draft/painting.html#StrokeMiterlimitProperty
    types: [ 'positiveNumber' ]
  },
  'stroke-opacity': {
    // https://w3c.github.io/svgwg/svg2-draft/painting.html#StrokeOpacityProperty
    types: [ 'opacity' ]
  },
  'stroke-width': {
    // https://w3c.github.io/svgwg/svg2-draft/painting.html#StrokeWidthProperty
    types: [
    ]
  },
  'table-layout': {
    // https://drafts.csswg.org/css-tables/#propdef-table-layout
    types: [
      { type: 'discrete', options: [ [ 'auto', 'fixed' ] ] }
    ]
  },
  'text-align': {
    // https://drafts.csswg.org/css-text-3/#propdef-text-align
    types: [
      { type: 'discrete', options: [ [ 'start', 'end' ] ] }
    ]
  },
  'text-align-last': {
    // https://drafts.csswg.org/css-text-3/#propdef-text-align-last
    types: [
      { type: 'discrete', options: [ [ 'start', 'end' ] ] }
    ]
  },
  'text-anchor': {
    // https://w3c.github.io/svgwg/svg2-draft/text.html#TextAnchorProperty
    types: [
      { type: 'discrete', options: [ [ 'middle', 'end' ] ] }
    ]
  },
  'text-autospace': {
    // https://drafts.csswg.org/css-text-4/#text-autospace-property
    types: [
      { type: 'discrete', options: [ [ 'normal', 'no-autospace' ] ] }
    ]
  },
  'text-box-edge': {
    // https://drafts.csswg.org/css-inline-3/#text-edges
    types: [
      { type: 'discrete', options: [ [ 'auto', 'text' ] ] }
    ]
  },
  'text-box-trim': {
    // https://drafts.csswg.org/css-inline-3/#propdef-text-box-trim
    types: [
      { type: 'discrete', options: [ [ 'none', 'trim-start' ] ] }
    ]
  },
  'text-decoration': {
    // https://drafts.csswg.org/css-text-decor-3/#propdef-text-decoration
    types: [
      { type: 'discrete', options: [ [ 'underline', 'overline' ] ] }
    ]
  },
  'text-decoration-color': {
    // https://drafts.csswg.org/css-text-decor-3/#propdef-text-decoration-color
    types: [ 'color' ]
  },
  'text-decoration-line': {
    // https://drafts.csswg.org/css-text-decor-3/#propdef-text-decoration-line
    types: [
      { type: 'discrete', options: [ [ 'underline', 'overline' ] ] }
    ]
  },
  'text-decoration-skip': {
    // https://drafts.csswg.org/css-text-decor-4/#propdef-text-decoration-skip
    types: [
      { type: 'discrete', options: [ [ 'auto', 'none' ] ] }
    ]
  },
  'text-decoration-skip-ink': {
    // https://drafts.csswg.org/css-text-decor-4/#propdef-text-decoration-skip-ink
    types: [
      { type: 'discrete', options: [ [ 'auto', 'none' ] ] }
    ]
  },
  'text-decoration-style': {
    // http://dev.w3.org/csswg/css-text-decor-3/#propdef-text-decoration-style
    types: [
      { type: 'discrete', options: [ [ 'solid', 'dotted' ] ] }
    ]
  },
  'text-emphasis-color': {
    // https://drafts.csswg.org/css-text-decor-3/#propdef-text-emphasis-color
    types: [ 'color' ]
  },
  'text-emphasis-position': {
    // http://dev.w3.org/csswg/css-text-decor-3/#propdef-text-emphasis-position
    types: [
      { type: 'discrete', options: [ [ 'over', 'under left' ] ] }
    ]
  },
  'text-emphasis-style': {
    // http://dev.w3.org/csswg/css-text-decor-3/#propdef-text-emphasis-style
    types: [
      { type: 'discrete', options: [ [ 'circle', 'open dot' ] ] }
    ]
  },
  'text-group-align': {
    // https://drafts.csswg.org/css-text-4/#propdef-text-group-align
    types: [
      { type: 'discrete', options: [ [ 'none', 'center' ] ] }
    ]
  },
  'text-indent': {
    // https://drafts.csswg.org/css-text-3/#propdef-text-indent
    types: [
    ]
  },
  'text-overflow': {
    // https://drafts.csswg.org/css-ui/#propdef-text-overflow
    types: [
      { type: 'discrete', options: [ [ 'clip', 'ellipsis' ] ] }
    ]
  },
  'text-rendering': {
    // https://w3c.github.io/svgwg/svg2-draft/painting.html#TextRenderingProperty
    types: [
      { type: 'discrete', options: [ [ 'optimizeSpeed', 'optimizeLegibility' ] ] }
    ]
  },
  'text-shadow': {
    // https://drafts.csswg.org/css-text-decor-3/#propdef-text-shadow
    types: [ 'textShadowList' ],
    setup: t => {
      const element = createElement(t);
      element.style.color = 'green';
      return element;
    }
  },
  'text-spacing-trim': {
    // https://drafts.csswg.org/css-text-4/#text-spacing-trim-property
    types: [
      { type: 'discrete', options: [ [ 'normal', 'space-all' ] ] }
    ]
  },
  'text-transform': {
    // https://drafts.csswg.org/css-text-3/#propdef-text-transform
    types: [
      { type: 'discrete', options: [ [ 'capitalize', 'uppercase' ] ] }
    ]
  },
  'text-wrap': {
    // https://drafts.csswg.org/css-text-4/#propdef-text-wrap
    types: [
      { type: 'discrete', options: [ [ 'wrap', 'nowrap' ] ] }
    ]
  },
  'touch-action': {
    // https://w3c.github.io/pointerevents/#the-touch-action-css-property
    types: [
      { type: 'discrete', options: [ [ 'auto', 'none' ] ] }
    ]
  },
  'top': {
    // https://drafts.csswg.org/css-position/#propdef-top
    types: [
    ]
  },
  'transform': {
    // https://drafts.csswg.org/css-transforms/#propdef-transform
    types: [ 'transformList' ]
  },
  'transform-box': {
    // https://drafts.csswg.org/css-transforms/#propdef-transform-box
    types: [
      { type: 'discrete', options: [ [ 'fill-box', 'border-box' ] ] }
    ]
  },
  'transform-origin': {
    // https://drafts.csswg.org/css-transforms/#propdef-transform-origin
    types: [
    ]
  },
  'transform-style': {
    // https://drafts.csswg.org/css-transforms/#propdef-transform-style
    types: [
      { type: 'discrete', options: [ [ 'flat', 'preserve-3d' ] ] }
    ]
  },
  'rotate': {
    // https://drafts.csswg.org/css-transforms-2/#individual-transforms
    types: [ 'rotateList' ]
  },
  'translate': {
    // https://drafts.csswg.org/css-transforms-2/#individual-transforms
    types: [ 'translateList' ],
    setup: t => {
      // We need to set a width/height for resolving percentages against.
      const element = createElement(t);
      element.style.width = '100px';
      element.style.height = '100px';
      return element;
    }
  },
  'scale': {
    // https://drafts.csswg.org/css-transforms-2/#individual-transforms
    types: [ 'scaleList' ]
  },
  'vector-effect': {
    // https://w3c.github.io/svgwg/svg2-draft/coords.html#VectorEffectProperty
    types: [
      { type: 'discrete', options: [ [ 'none', 'non-scaling-stroke' ] ] },
    ]
  },
  'vertical-align': {
    // https://drafts.csswg.org/css21/visudet.html#propdef-vertical-align
    types: [
    ]
  },
  'view-transition-class': {
    // https://drafts.csswg.org/css-view-transitions/#propdef-view-transition-name
    types: [
      { type: 'discrete', options: [ [ 'none', 'card scale-animation' ] ] },
    ]
  },
  'view-transition-name': {
    // https://drafts.csswg.org/css-view-transitions/#propdef-view-transition-name
    types: [
      { type: 'discrete', options: [ [ 'none', 'header' ] ] },
    ]
  },
  'visibility': {
    // https://drafts.csswg.org/css2/visufx.html#propdef-visibility
    types: [ 'visibility' ]
  },
  'white-space': {
    // https://drafts.csswg.org/css-text-4/#propdef-white-space
    types: [
      { type: 'discrete', options: [ [ 'pre', 'nowrap' ] ] }
    ]
  },
  'white-space-collapse': {
    // https://drafts.csswg.org/css-text-4/#propdef-white-space-collapse
    types: [
      { type: 'discrete', options: [ [ 'collapse', 'preserve' ] ] }
    ]
  },
  'width': {
    // https://drafts.csswg.org/css21/visudet.html#propdef-width
    types: [
    ]
  },
  'word-break': {
    // https://drafts.csswg.org/css-text-3/#propdef-word-break
    types: [
      { type: 'discrete', options: [ [ 'keep-all', 'break-all' ] ] }
    ]
  },
  'word-spacing': {
    // https://drafts.csswg.org/css-text-3/#propdef-word-spacing
    types: [ 'lengthPercentageOrCalc' ]
  },
  'z-index': {
    // https://drafts.csswg.org/css-position/#propdef-z-index
    types: [
    ]
  },
};

function testAnimationSamples(animation, idlName, testSamples) {
  const pseudoType = animation.effect.pseudoElement;
  const target = animation.effect.target;
  for (const testSample of testSamples) {
    animation.currentTime = testSample.time;
    assert_equals(getComputedStyle(target, pseudoType)[idlName].toLowerCase(),
                  testSample.expected,
                  `The value should be ${testSample.expected}` +
                  ` at ${testSample.time}ms`);
  }
}

function toOrderedArray(string) {
  return string.split(/\s*,\s/).sort();
}

// This test is for some list-based CSS properties such as font-variant-settings
// don't specify an order for serializing computed values.
// This test is for such the property.
function testAnimationSamplesWithAnyOrder(animation, idlName, testSamples) {
  const type = animation.effect.pseudoElement;
  const target = animation.effect.target;
  for (const testSample of testSamples) {
    animation.currentTime = testSample.time;

    // Convert to array and sort the expected and actual value lists first
    // before comparing them.
    const computedValues =
      toOrderedArray(getComputedStyle(target, type)[idlName]);
    const expectedValues = toOrderedArray(testSample.expected);

    assert_array_equals(computedValues, expectedValues,
                        `The computed values should be ${expectedValues}` +
                        ` at ${testSample.time}ms`);
  }
}

function RoundMatrix(style) {
  var matrixMatch = style.match(/^(matrix(3d)?)\(.+\)$/);
  if (!!matrixMatch) {
    var matrixType = matrixMatch[1];
    var matrixArgs = style.substr(matrixType.length);
    var extractmatrix = function(matrixStr) {
      var list = [];
      var regex = /[+\-]?[0-9]+[.]?[0-9]*(e[+/-][0-9]+)?/g;
      var match = undefined;
      do {
        match = regex.exec(matrixStr);
        if (match) {
          list.push(parseFloat(parseFloat(match[0]).toFixed(6)));
        }
      } while (match);
      return list;
    }
    return matrixType + '(' + extractmatrix(matrixArgs).join(', ') + ')';
  }
  return style;
}

function testAnimationSampleMatrices(animation, idlName, testSamples) {
  const target = animation.effect.target;
  for (const testSample of testSamples) {
    animation.currentTime = testSample.time;
    const actual = RoundMatrix(getComputedStyle(target)[idlName]);
    const expected = RoundMatrix(createMatrixFromArray(testSample.expected));
    assert_matrix_equals(actual, expected,
                         `The value should be ${expected} at`
                         + ` ${testSample.time}ms but got ${actual}`);
  }
}

function testAnimationSampleRotate3d(animation, idlName, testSamples) {
  const target = animation.effect.target;
  for (const testSample of testSamples) {
    animation.currentTime = testSample.time;
    const actual = getComputedStyle(target)[idlName];
    const expected = testSample.expected;
    assert_rotate3d_equals(actual, expected,
                         `The value should be ${expected} at`
                         + ` ${testSample.time}ms but got ${actual}`);
  }
}

function createTestElement(t, setup) {
  return setup ? setup(t) : createElement(t);
}

function isSupported(property) {
  const testKeyframe = new TestKeyframe(propertyToIDL(property));
  assert_not_equals(window.KeyframeEffect, undefined, 'window.KeyframeEffect');
  try {
    // Since TestKeyframe returns 'undefined' for |property|,
    // the KeyframeEffect constructor will throw
    // if the string 'undefined' is not a valid value for the property.
    new KeyframeEffect(null, testKeyframe);
  } catch(e) {}
  return testKeyframe.propAccessCount !== 0;
}

function TestKeyframe(testProp) {
  let _propAccessCount = 0;

  Object.defineProperty(this, testProp, {
    get: function() { _propAccessCount++; },
    enumerable: true
  });

  Object.defineProperty(this, 'propAccessCount', {
    get: function() { return _propAccessCount; }
  });
}

function propertyToIDL(property) {
  // https://drafts.csswg.org/web-animations/#animation-property-name-to-idl-attribute-name
  if (property === 'float') {
    return 'cssFloat';
  }
  return property.replace(/-[a-z]/gi,
                          function (str) {
                            return str.substr(1).toUpperCase(); });
}
function calcFromPercentage(idlName, percentageValue) {
  const examElem = document.createElement('div');
  document.body.appendChild(examElem);
  examElem.style[idlName] = percentageValue;

  const calcValue = getComputedStyle(examElem)[idlName];
  document.body.removeChild(examElem);

  return calcValue;
}
