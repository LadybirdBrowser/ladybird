window.test_driver_internal.get_computed_label = async function(element) {
    return await window.internals.getComputedLabel(element);
};

window.test_driver_internal.get_computed_role = async function(element) {
    return await window.internals.getComputedRole(element);
};
