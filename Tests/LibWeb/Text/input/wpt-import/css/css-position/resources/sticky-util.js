/**
 * Builds a generic structure that looks like:
 *
 * <div class="scroller">  // 100x200 viewport
 *   <div class="contents">  // 100x500
 *     <div class="prepadding"></div> // 100x100
 *     <div class="container">  // 300x300 containing block
 *       <div class="filler"></div>  // 100x100
 *       <div class="sticky box"></div>  // 100x100
 *     </div>
 *   </div>
 * </div>
 *
 * If the sticky direction is 'left' or 'right', the necessary blocks will be
 * marked as inline-block and the dimensions above are flipped.
 *
 * Returns an 'elements' object which has each of the above elements as an
 * accessible property.
 */
function setupStickyTest(stickyDirection, stickyOffset) {
  const elements = {};
  const inline = stickyDirection === 'left' || stickyDirection === 'right';

  elements.scroller = document.createElement('div');
  elements.scroller.style.position = 'relative';
  elements.scroller.style.width = (inline ? '200px' : '100px');
  elements.scroller.style.height = (inline ? '100px' : '200px');

  // 'hidden' is used here instead of 'scroll' because this prevents
  // scrollbars from affecting the size and offset of sticky items.
  elements.scroller.style.overflow = 'hidden';

  elements.contents = document.createElement('div');
  elements.contents.style.height = (inline ? '100%' : '500px');
  elements.contents.style.width = (inline ? '500px' : '100%');

  elements.prepadding = document.createElement('div');
  elements.prepadding.style.height = (inline ? '100%' : '100px');
  elements.prepadding.style.width = (inline ? '100px' : '100%');
  if (inline)
    elements.prepadding.style.display = 'inline-block';

  elements.container = document.createElement('div');
  elements.container.style.height = (inline ? '100%' : '300px');
  elements.container.style.width = (inline ? '300px' : '100%');
  if (inline)
    elements.container.style.display = 'inline-block';

  elements.filler = document.createElement('div');
  elements.filler.style.height = (inline ? '100%' : '100px');
  elements.filler.style.width = (inline ? '100px' : '100%');
  if (inline)
    elements.filler.style.display = 'inline-block';

  elements.sticky = document.createElement('div');
  elements.sticky.style = `${stickyDirection}: ${stickyOffset}px;`;
  elements.sticky.style.position = 'sticky';
  elements.sticky.style.height = (inline ? '100%' : '100px');
  elements.sticky.style.width = (inline ? '100px' : '100%');
  elements.sticky.style.backgroundColor = 'green';
  if (inline)
    elements.sticky.style.display = 'inline-block';

  elements.scroller.appendChild(elements.contents);
  elements.contents.appendChild(elements.prepadding);
  elements.contents.appendChild(elements.container);
  elements.container.appendChild(elements.filler);
  elements.container.appendChild(elements.sticky);

  document.body.appendChild(elements.scroller);

  return elements;
}

/**
 * Similar to above, but nests a second sticky (named innerSticky) inside the
 * sticky element.
 *
 * In the 'bottom' and 'right' cases, we also inject some padding before the
 * innerSticky element, to give it something to push into. This inner padding is
 * not exposed.
 */
function setupNestedStickyTest(stickyDirection, outerStickyOffset,
    innerStickyOffset) {
  const elements = setupStickyTest(stickyDirection, outerStickyOffset);

  const inline = stickyDirection === 'left' || stickyDirection === 'right';
  if (stickyDirection === 'bottom' || stickyDirection === 'right') {
    const innerPadding = document.createElement('div');
    innerPadding.style.height = (inline ? '100%' : '50px');
    innerPadding.style.width = (inline ? '50px' : '100%');
    if (inline)
      innerPadding.style.display = 'inline-block';
    elements.sticky.appendChild(innerPadding);
  }

  elements.innerSticky = document.createElement('div');
  elements.innerSticky.style = `${stickyDirection}: ${innerStickyOffset}px;`;
  elements.innerSticky.style.position = 'sticky';
  elements.innerSticky.style.height = (inline ? '100%' : '50px');
  elements.innerSticky.style.width = (inline ? '50px' : '100%');
  elements.innerSticky.style.backgroundColor = 'blue';
  if (inline)
    elements.innerSticky.style.display = 'inline-block';

  elements.sticky.appendChild(elements.innerSticky);

  return elements;
}
