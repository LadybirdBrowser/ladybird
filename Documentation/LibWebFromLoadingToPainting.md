## LibWeb: From loading to painting

**NOTE: This document is a work in progress!**

FIXME: Add more information about everything.

FIXME: Add lots of links to relevant specs, etc.

### Resource loading

We make an IPC call to RequestServer, asking it to start loading the URL we've been asked to load.

### HTML parsing

The job of the HTML parser is to take an input stream, possibly determine its encoding, and then parse it into a DOM tree. For historical reasons, this is unusually complicated.

LibWeb implements the HTML tokenization and parsing algorithms as laid out in the HTML specification. While the spec is detailed, it's also highly idiosyncratic. The tokenizer and parser reach into each other and update the other's state in certain situations.

Due to the `document.write()` API, the parser also allows programmatic injection of new input into the parser from JavaScript. The document being parsed may run JavaScript, and this JavaScript may indirectly interact with the parser by feeding it override inputs.

### CSS parsing

* CSS parser
* Builds the CSSOM
* Values with variables can't be resolved until cascade, kept as "unresolved" values

### JS parsing & execution

* JS parser
* Builds JavaScript AST
* Interpreter runs AST
* Garbage collector (basic stop-the-world mark&sweep)

### Style computation

This step is concerned with finding the set of CSS values that apply to a given DOM element.

#### Selector matching

A CSS selector is represented by the CSS::Selector class.

Given the following selector:

```css
    #foo .bar.baz > img
```

We get the following C++ object tree:

```
* CSS::Selector
  * CSS::CompoundSelector (combinator: ImmediateChild)
    * CSS::SimpleSelector (type: TagName, value: img)
  * CSS::CompoundSelector (combinator: Descendant)
    * CSS::SimpleSelector (type: Class, value: bar)
    * CSS::SimpleSelector (type: Class, value: baz)
  * CSS::CompoundSelector (combinator: None)
    * CSS::SimpleSelector (type: ID)
```

Selectors are evaluated right-to-left, and we're looking for the first opportunity to reject it.

##### Optimization notes

We try to minimize the number of selectors that have to be evaluated for each DOM element.
The main optimization is a cache in StyleComputer that divides style rules into buckets based on what their rightmost complex selector must match. For example, a selector that can only match an element with the class "foo" will only ever be evaluated against elements that currently have the class "foo".

#### Cascading to the final values

The C in CSS is for "cascading" and "the cascade" refers to the process where all the CSS declarations that should apply to a DOM element are evaluated in order. The order is determined by a number of factors, such as selector specificity, rule order within the style sheet. There's also per-property consideration of the `!important` annotation, which allows authors to override the normal cascade order.

* To compute the "computed style" for an element...
* Perform the CSS cascade to determine final property values
* Interpolate variables
* Absolutize, etc

We separate CSS rules by their cascade origin. The two origins we're concerned with are "user-agent" and "author". If we supported custom user style sheets (we don't), they'd need a separate cascade origin as well.

The cascade origin determines the processing order for rules. The "user-agent" style is the least important, so it gets processed first. Then author style is added on top of that.

Note: the user-agent style is a built-in CSS style sheet that lives in the LibWeb source code [here](https://github.com/LadybirdBrowser/ladybird/blob/master/Userland/Libraries/LibWeb/CSS/Default.css).

The end product of style computation is a fully populated StyleProperties object. It has a CSSStyleValue for each CSS::PropertyID. In spec parlance, these are the *computed* values. (Note that these are not the same as you get from `getComputedStyle()`, that API returns the *resolved* values.)

#### Resolving CSS custom properties ("variables")

Custom properties are resolved during the cascade. This is the earliest possible time where this can be done, as the value of a given variable depends on the result of the cascade.

### Building the layout tree

To prepare for layout, we combine the DOM with the results of style computation to create a new tree: the layout tree.
This is essentially the "box tree" in the CSS specifications, with some important differences that allow us to make better use of the C++ type system.

There isn't a 1:1 mapping between DOM nodes and layout nodes. Elements with `display:none` style don't generate a layout node at all.

A number of tree fix-ups are also performed:

- To maintain the invariant that block containers only have all block-level children or all inline-level children, inline-level boxes with block-level siblings are wrapped in anonymous wrapper boxes.
- If an individual table component box is found outside the context of a full table, a set of anonymous table boxes are generated to compensate and ensure that there's always a fully-formed table. (FIXME: This is currently buggy..)
- For list item boxes (`display: list-item`), a box representing the marker is inserted if needed.

### Layout

Layout starts at the ICB (initial containing block), which is the layout node that corresponds to the DOM document node.
CSS says that the ICB should be the size of the viewport, so the very first thing layout does is assign these dimensions.

Layout works through a mechanism call formatting contexts. CSS defines a number of different formatting contexts:
- Block Formatting Context (BFC)
- Inline Formatting Context (IFC)
- Table Formatting Context (TFC)
- Flex Formatting Context (FFC)

In LibWeb, to simplify the layout model, we also define our own SVGFormattingContext for embedded SVG content. This isn't part of the SVG specification, but simplifies the implementation.

Layout is fundamentally recursive. We start at the ICB, which always creates a BFC.

#### Block-level layout

BFC works by laying out its children, one by one in tree order. If its children are block level, they are laid out along the block axis of the BFC's root box. Block-axis margins between adjacent boxes are collapsed.

Floating boxes (CSS `float:left` or `float:right`) are interesting. First, we compute where they would have ended up if they weren't floating. Then we push them towards the left or right edge as requested. If they collide with the edge of another floating box before getting to the edge, they are stacked next to that box, forming a sort of "float line". BFC keeps track of floating boxes on both sides.

#### Inline-level layout

If its children are inline level, BFC creates an IFC to handle them. Unlike other formatting contexts, IFC can't work alone; it always works together with its parent BFC.

The job of IFC is to generate line boxes. Line boxes are essentially a sequence of inline content fragments, laid out along the inline axis. Line boxes are stored in the IFC's containing block box.

There are three main classes involved in inline-level layout:

- InlineFormattingContext (IFC)
- LineBuilder
- InlineLevelIterator

IFC drives the high-level loop by creating a LineBuilder and an InlineLevelIterator. It then traverses the inline-level content within the IFC containing block, one item at a time, by calling InlineLevelIterator::next().

The items produced by InlineLevelIterator are then passed along to LineBuilder, which adds them to the line box we're currently building.
When a line box is filled up, we insert a break and begin a new box after it.

We always keep track of how much space is available on the current line. When starting a new line, this is reset by computing the width of the IFC's containing block and then subtracting the size occupied by floating boxes on both sides. We get this information by querying the parent BFC about floating boxes intersecting the Y coordinate of the new line.

The result of performing a layout is a LayoutState object. This object contains the CSS "used values" (final metrics, including line boxes) for all box that were in scope of the layout. The LayoutState can either be committed (via `commit()`) or simply discarded. This allows us to perform non-destructive (or "immutable") layouts if we're only interested in measuring something.

### Paintable and the paint tree

When layout is finished, we take all the final metrics for each box and generate a new tree: the paint tree. The paint tree hangs off of the layout tree, and you can reach the corresponding paintable for a given layout node via `Layout::Node::paintable()`. There's also a convenience accessor in the DOM: `DOM::Node::paintable()`.

Unlike Layout::Node (which is basically a box tree node with associated style), Paintable is an object with fully finalized metrics. It has two jobs, painting and hit testing.

Note that not every layout node will have a paintable. If a layout node cannot possibly be visible, it may not require a paintable.

Every paintable has a corresponding layout node, and painting code will reach into the layout node for some information, to avoid having it in multiple places.

### Stacking contexts

There's one final thing we have to do before we can paint: we have to create stacking contexts.

Stacking contexts are a 3-dimensional model of layers (stacking contexts) that places content along a Z axis. This is what the CSS `z-index` property is for.

The set of rules for what becomes a stacking context are somewhat intricate, but the important thing is that we create a new tree: the stacking context tree.

The stacking context tree is rooted at the ICB, and can have zero or more descendants. Each descendant stacking context has a corresponding layout that it's attached to.

### Painting

Painting follows the order specified in the CSS2 appendix E. The rules are quite involved, but two main things to know about:

- Painting is driven through stacking contexts, which are painted back-to-front (stacking context tree order)
- Painting is performed in *phases*. For each stacking context, we paint in order: backgrounds & borders, floats, backgrounds & borders for inline and replaced content, foreground (text), focus outlines and overlays.
