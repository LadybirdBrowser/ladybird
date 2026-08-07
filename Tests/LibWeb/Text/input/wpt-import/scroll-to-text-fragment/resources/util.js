// Returns true if element's center is within the visual viewport bounds.
function isInViewport(element) {
  const viewportRect = {
    left: visualViewport.offsetLeft,
    top: visualViewport.offsetTop,
    right: visualViewport.offsetLeft + visualViewport.width,
    bottom: visualViewport.offsetTop + visualViewport.height
  };

  const elementRect = element.getBoundingClientRect();
  const elementCenter = {
    x: elementRect.left + elementRect.width / 2,
    y: elementRect.top + elementRect.height / 2
  };

  return elementCenter.x > viewportRect.left &&
         elementCenter.x < viewportRect.right &&
         elementCenter.y > viewportRect.top &&
         elementCenter.y < viewportRect.bottom;
}

