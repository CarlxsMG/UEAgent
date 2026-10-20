/* The tool explorer (docs/explorer.md). It searches and filters assets/catalog/tools.json, which
   Tools/mkdocs_hooks.py writes from Resources/command_catalog.json when the site is built. Every
   string from the catalog is set as text, never parsed as HTML. */
(() => {
  "use strict";

  const STEP = 50; // results shown at first, and added by each "Show more"
  const EFFECTS = [["", "Any"], ["r", "Read-only"], ["a", "Additive"], ["d", "Destructive"]];
  const FLAG_TEXT = { r: "Read-only", a: "Additive", d: "Destructive", i: "Idempotent", o: "Open world", p: "Refused during PIE" };
  const FLAG_KIND = { r: "read-only", a: "additive", d: "destructive", i: "idempotent", o: "open-world", p: "pie" };
  const PLACEHOLDER = { string: "", number: 0, integer: 0, boolean: false, array: [], object: {} };

  let catalog = null; // loaded once, and kept across the theme's instant navigation

  function h(tag, props, ...children) {
    const node = document.createElement(tag);
    for (const [key, value] of Object.entries(props || {})) {
      if (value === undefined || value === null || value === false) continue;
      if (key === "class") node.className = value;
      else if (key === "text") node.textContent = value;
      else if (key.startsWith("on")) node.addEventListener(key.slice(2), value);
      else node.setAttribute(key, value === true ? "" : value);
    }
    for (const child of children.flat()) {
      if (child !== null && child !== undefined && child !== false) node.append(child);
    }
    return node;
  }

  /** Catalog text with its `code spans` as <code> elements. */
  function prose(text) {
    return String(text).split(/(`[^`\n]+`)/).filter(Boolean).map((part) =>
      /^`.+`$/.test(part) ? h("code", { text: part.slice(1, -1) }) : part);
  }

  function chips(flags) {
    return h("span", { class: "ua-chips" }, [...flags].map((flag) =>
      h("span", { class: `ua-chip ua-chip--${FLAG_KIND[flag]}`, text: FLAG_TEXT[flag] })));
  }

  function firstSentence(text) {
    const match = /^.*?[.!?](\s|$)/s.exec(text);
    const sentence = (match ? match[0] : text).trim();
    return sentence.length > 180 ? `${sentence.slice(0, 177).replace(/\s+\S*$/, "")}...` : sentence;
  }

  async function load(url) {
    if (!catalog) {
      const response = await fetch(url);
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const raw = await response.json();
      catalog = {
        domains: raw.domains,
        summaries: raw.summaries,
        tools: raw.tools.map(([name, domain, flags, description, params, returns]) => ({
          name, flags, description, returns,
          domain: raw.domains[domain],
          params: params.map(([pname, type, required, about]) => ({ name: pname, type, required: required === 1, about })),
          summary: firstSentence(description),
          haystack: `${name} ${name.replace(/_/g, " ")} ${description} ${params.map((p) => p[0]).join(" ")}`.toLowerCase(),
        })),
      };
    }
    return catalog;
  }

  function score(tool, terms) {
    if (!terms.length) return 1;
    if (!terms.every((term) => tool.haystack.includes(term))) return 0;
    const joined = terms.join("_");
    let points = tool.name === joined ? 1000 : tool.name.startsWith(joined) ? 300 : tool.name.includes(joined) ? 200 : 0;
    for (const term of terms) if (tool.name.includes(term)) points += 50;
    return points + 1;
  }

  function readState() {
    const params = new URLSearchParams(location.search);
    return {
      query: params.get("query") || "", // not "q": the theme opens its own search on ?q=
      domain: params.get("domain") || "",
      effect: EFFECTS.some(([value]) => value === params.get("effect")) ? params.get("effect") : "",
      idempotent: params.get("idempotent") === "1",
      pieSafe: params.get("pie") === "safe",
      open: params.get("tool") || "",
    };
  }

  function writeState(state) {
    const params = new URLSearchParams();
    if (state.query) params.set("query", state.query);
    if (state.domain) params.set("domain", state.domain);
    if (state.effect) params.set("effect", state.effect);
    if (state.idempotent) params.set("idempotent", "1");
    if (state.pieSafe) params.set("pie", "safe");
    if (state.open) params.set("tool", state.open);
    const search = params.toString();
    history.replaceState(history.state, "", `${location.pathname}${search ? `?${search}` : ""}${location.hash}`);
  }

  async function copy(button, text) {
    const label = button.textContent;
    try {
      await navigator.clipboard.writeText(text);
      button.textContent = "Copied";
    } catch {
      button.textContent = "Copy failed";
    }
    setTimeout(() => { button.textContent = label; }, 1500);
  }

  function details(tool, reference) {
    const call = { name: tool.name, arguments: {} };
    for (const param of tool.params) {
      if (param.required) call.arguments[param.name] = PLACEHOLDER[param.type] ?? null;
    }
    const callText = JSON.stringify(call, null, 2);
    const share = `${location.origin}${location.pathname}?tool=${encodeURIComponent(tool.name)}`;
    return [
      h("p", {}, prose(tool.description)),
      tool.params.length
        ? h("div", { class: "md-typeset__scrollwrap" }, h("div", { class: "md-typeset__table" }, h("table", {},
          h("thead", {}, h("tr", {}, ["Parameter", "Type", "Required", "Description"].map((title) => h("th", { text: title })))),
          h("tbody", {}, tool.params.map((param) => h("tr", {},
            h("td", {}, h("code", { text: param.name })),
            h("td", { text: param.type }),
            h("td", { text: param.required ? "yes" : "no" }),
            h("td", {}, prose(param.about))))))))
        : h("p", { text: "No parameters." }),
      tool.returns ? h("p", {}, h("strong", { text: "Returns: " }), h("code", { text: tool.returns })) : null,
      h("p", { class: "ua-x-label", text: "A call, with its required arguments:" }),
      h("pre", { class: "ua-x-call" }, h("code", { text: callText })),
      h("div", { class: "ua-x-actions" },
        h("button", { type: "button", class: "md-button md-button--primary", text: "Copy the call", onclick: (event) => copy(event.currentTarget, callText) }),
        h("a", { class: "md-button", href: new URL(`${tool.domain}/#${tool.name}`, reference).href, text: "In the reference" }),
        h("button", { type: "button", class: "md-button", text: "Copy a link", onclick: (event) => copy(event.currentTarget, share) })),
    ];
  }

  function build(root, data, reference) {
    const state = readState();
    const counts = Object.fromEntries(data.domains.map((domain) => [domain, 0]));
    for (const tool of data.tools) counts[tool.domain] += 1;
    if (!(state.domain in counts)) state.domain = "";
    if (state.open && !data.tools.some((tool) => tool.name === state.open)) state.open = "";

    const search = h("input", {
      type: "search", value: state.query, autocomplete: "off", spellcheck: "false",
      placeholder: "A name, or what it does: spawn actor, landscape, montage...",
    });
    const domain = h("select", {},
      h("option", { value: "", text: `All domains (${data.tools.length})` }),
      data.domains.map((name) => h("option", { value: name, text: `${name} (${counts[name]})` })));
    domain.value = state.domain;
    const effects = EFFECTS.map(([value, text]) => {
      const input = h("input", { type: "radio", name: "ua-x-effect", value });
      input.checked = state.effect === value;
      return h("label", {}, input, h("span", { text }));
    });
    const idempotent = h("input", { type: "checkbox" });
    idempotent.checked = state.idempotent;
    const pieSafe = h("input", { type: "checkbox" });
    pieSafe.checked = state.pieSafe;

    const about = h("p", { class: "ua-x-about" });
    const count = h("p", { class: "ua-x-count", "aria-live": "polite" });
    const list = h("ol", { class: "ua-x-list" });
    const more = h("button", { type: "button", class: "md-button ua-x-more" });
    const empty = h("p", { class: "ua-x-empty", hidden: true }, "No tool matches. ",
      h("button", { type: "button", class: "ua-x-link", text: "Clear the search and the filters", onclick: reset }));

    root.replaceChildren(
      h("div", { class: "ua-x-controls", role: "search" },
        h("label", { class: "ua-x-field ua-x-field--search" }, h("span", { text: "Search" }), search),
        h("label", { class: "ua-x-field" }, h("span", { text: "Domain" }), domain),
        h("fieldset", { class: "ua-x-effect" }, h("legend", { text: "What a call does to the project" }), effects),
        h("div", { class: "ua-x-checks" },
          h("label", {}, idempotent, h("span", { text: "Idempotent only" })),
          h("label", {}, pieSafe, h("span", { text: "Hide the tools refused during PIE" })))),
      about, count, list, more, empty);

    let results = [];
    let shown = 0;

    function item(tool) {
      const id = `ua-x-${tool.name}`;
      const body = h("div", { class: "ua-x-body", id, hidden: true });
      const head = h("button", { type: "button", class: "ua-x-head", "aria-expanded": "false", "aria-controls": id },
        h("span", { class: "ua-x-title" }, h("code", { class: "ua-x-name", text: tool.name }), h("span", { class: "ua-x-domain", text: tool.domain })),
        chips(tool.flags),
        h("span", { class: "ua-x-summary" }, prose(tool.summary)));
      function toggle(open) {
        if (open && !body.childElementCount) body.append(...details(tool, reference));
        body.hidden = !open;
        head.setAttribute("aria-expanded", String(open));
        state.open = open ? tool.name : (state.open === tool.name ? "" : state.open);
        writeState(state);
      }
      head.addEventListener("click", () => toggle(body.hidden));
      const node = h("li", { class: "ua-x-item" }, head, body);
      node.open = () => toggle(true);
      return node;
    }

    function showMore() {
      const next = results.slice(shown, shown + STEP);
      list.append(...next.map(item));
      shown += next.length;
      const left = results.length - shown;
      more.hidden = left <= 0;
      more.textContent = `Show ${Math.min(left, STEP)} more`;
    }

    /** Filters and sorts the tools; a change of search or filter closes the tool the link opened. */
    function apply(keepOpen = false) {
      if (!keepOpen) state.open = "";
      const terms = state.query.toLowerCase().split(/[\s_]+/).filter(Boolean);
      results = data.tools
        .map((tool) => [tool, score(tool, terms)])
        .filter(([tool, points]) => points > 0
          && (!state.domain || tool.domain === state.domain)
          && (!state.effect || tool.flags.includes(state.effect))
          && (!state.idempotent || tool.flags.includes("i"))
          && (!state.pieSafe || !tool.flags.includes("p")))
        .sort((a, b) => b[1] - a[1] || a[0].name.localeCompare(b[0].name))
        .map(([tool]) => tool);
      if (state.open) {
        const at = results.findIndex((tool) => tool.name === state.open);
        if (at > 0) results.unshift(...results.splice(at, 1));
      }
      list.replaceChildren();
      shown = 0;
      showMore();
      count.textContent = results.length === data.tools.length
        ? `${data.tools.length} tools`
        : `${results.length} of ${data.tools.length} tools`;
      empty.hidden = results.length > 0;
      about.replaceChildren();
      if (state.domain) {
        const at = data.domains.indexOf(state.domain);
        about.append(h("strong", { text: `${state.domain}: ` }), prose(data.summaries[at]), ". ",
          h("a", { href: new URL(`${state.domain}/`, reference).href, text: "The domain in the reference" }), ".");
      }
      writeState(state);
    }

    function reset() {
      Object.assign(state, { query: "", domain: "", effect: "", idempotent: false, pieSafe: false, open: "" });
      search.value = "";
      domain.value = "";
      effects.forEach((label) => { label.firstChild.checked = label.firstChild.value === ""; });
      idempotent.checked = false;
      pieSafe.checked = false;
      apply();
      search.focus();
    }

    let typing = 0;
    search.addEventListener("input", () => {
      clearTimeout(typing);
      typing = setTimeout(() => { state.query = search.value.trim(); apply(); }, 120);
    });
    domain.addEventListener("change", () => { state.domain = domain.value; apply(); });
    effects.forEach((label) => label.firstChild.addEventListener("change", (event) => { state.effect = event.target.value; apply(); }));
    idempotent.addEventListener("change", () => { state.idempotent = idempotent.checked; apply(); });
    pieSafe.addEventListener("change", () => { state.pieSafe = pieSafe.checked; apply(); });
    more.addEventListener("click", showMore);

    apply(true);
    if (state.open && results.length && results[0].name === state.open) {
      const node = list.firstElementChild;
      node.open();
      node.scrollIntoView({ block: "start" });
    }
  }

  function start(root) {
    const source = new URL(root.dataset.catalog, location.href);
    const reference = new URL(root.dataset.reference, location.href);
    const fallback = [...root.childNodes];
    root.replaceChildren(h("p", { class: "ua-x-status", text: "Loading the catalog..." }));
    load(source.href)
      .then((data) => build(root, data, reference))
      .catch((error) => {
        root.replaceChildren(h("p", { class: "ua-x-status", text: `The catalog could not be loaded (${error.message}).` }), ...fallback);
      });
  }

  function init() {
    const root = document.getElementById("ua-explorer");
    if (root && !root.dataset.started) {
      root.dataset.started = "1";
      start(root);
    }
  }

  if (window.document$) window.document$.subscribe(init);
  else document.addEventListener("DOMContentLoaded", init);
})();
