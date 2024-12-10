window.test_driver_internal.click = function(element) {
    const boundingRect = element.getBoundingClientRect();
    const centerPoint = {
        x: boundingRect.left + boundingRect.width / 2,
        y: boundingRect.top + boundingRect.height / 2
    };
    window.internals.click(centerPoint.x, centerPoint.y);
    return Promise.resolve();
};

window.test_driver_internal.get_computed_label = async function(element) {
    return await window.internals.getComputedLabel(element);
};

window.test_driver_internal.get_computed_role = async function(element) {
    return await window.internals.getComputedRole(element);
};
