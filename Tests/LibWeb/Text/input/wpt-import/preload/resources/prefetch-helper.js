async function get_prefetch_info(href) {
    const response = await fetch(`${href}&mode=info`, {mode: "cors"});
    return await response.json();
}

async function prefetch(p = {}, t) {
    const link = document.createElement("link");
    link.rel = "prefetch";
    link.as = p.as;
    if (p.crossOrigin)
        link.setAttribute("crossorigin", p.crossOrigin);
    const uid = token();
    const params = new URLSearchParams();
    params.set("key", uid);
    for (const key in p)
        params.set(key, p[key]);
    const origin = p.origin || '';
    link.href = `${origin}/preload/resources/prefetch-info.py?${params.toString()}`;
    document.head.appendChild(link);
    while (!(await get_prefetch_info(link.href)).length) { }
    return {href: link.href, uid};
}
