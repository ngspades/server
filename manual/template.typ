// Shared layout of the manual: one definition for the HTML pages and the
// PDFs. The HTML stylesheet is emitted from here (tools/build_site.py moves
// it into site/style.css), so the look lives in one file.
#import "/chapters.typ": chapters
#import "/gen/verified.typ": verified

#let accent = rgb("#b3321e")
#let ink = rgb("#222222")
#let muted = rgb("#666666")
#let code-bg = rgb("#f4f1ec")

#let css = "
:root { --bg:#fbfaf8; --fg:#222; --muted:#666; --accent:#b3321e; --line:#ddd6cc; --nav:#f1ede6; --code:#f4f1ec; --link:#9a2a18; }
@media (prefers-color-scheme: dark) {
  :root { --bg:#17181a; --fg:#e6e3de; --muted:#9a968f; --accent:#ff7a5f; --line:#33353a; --nav:#1f2124; --code:#f4f1ec; --link:#ff9c86; }
}
* { box-sizing: border-box; }
body { margin:0; background:var(--bg); color:var(--fg); font:16px/1.6 -apple-system, 'Segoe UI', Helvetica, Arial, sans-serif; }
.layout { display:flex; min-height:100vh; }
.sidebar { width:290px; flex:none; background:var(--nav); border-right:1px solid var(--line); padding:20px 18px; position:sticky; top:0; height:100vh; overflow-y:auto; }
.sidebar .brand { font-weight:700; font-size:17px; color:var(--accent); display:block; margin-bottom:14px; text-decoration:none; }
.sidebar .part { text-transform:uppercase; letter-spacing:.06em; font-size:11px; color:var(--muted); margin:16px 0 4px; }
.sidebar a { display:block; padding:3px 8px; border-radius:5px; color:var(--fg); text-decoration:none; font-size:14px; }
.sidebar a:hover { background:var(--line); }
.sidebar a.current { background:var(--accent); color:#fff; }
main { flex:1; min-width:0; max-width:980px; padding:28px 40px 80px; }
h1 { font-size:2rem; margin:.2em 0 .6em; line-height:1.2; }
h3 { font-size:1.5rem; margin:2em 0 .5em; padding-bottom:.2em; border-bottom:1px solid var(--line); }
h4 { font-size:1.2rem; margin:1.6em 0 .4em; }
h5 { font-size:1.05rem; margin:1.8em 0 .3em; padding-top:.6em; border-top:1px dashed var(--line); }
a { color:var(--link); }
code { font:13.5px/1.4 ui-monospace, 'SF Mono', Menlo, Consolas, monospace; background:var(--code); color:#222; padding:1px 5px; border-radius:4px; }
pre { background:var(--code); color:#222; padding:12px 14px; border-radius:6px; overflow-x:auto; border:1px solid var(--line); }
pre code { background:none; padding:0; }
table { border-collapse:collapse; width:100%; margin:1em 0; font-size:14.5px; }
th, td { border:1px solid var(--line); padding:6px 9px; text-align:left; vertical-align:top; }
th { background:var(--nav); }
.tablewrap { overflow-x:auto; }
.callout { border-left:4px solid var(--accent); background:var(--nav); padding:8px 14px; margin:1em 0; border-radius:0 6px 6px 0; }
.callout.warning { border-left-color:#d9822b; }
.callout.tip { border-left-color:#2f8f5b; }
.callout .label { font-weight:700; text-transform:uppercase; font-size:12px; letter-spacing:.05em; display:block; }
.meta { font-size:13px; color:var(--muted); margin:.1em 0 .6em; }
.chip { display:inline-block; padding:0 8px; margin-right:6px; border-radius:10px; font-size:12px; border:1px solid var(--line); background:var(--nav); color:var(--fg); }
.chip.ok { border-color:#2f8f5b; color:#2f8f5b; }
.chip.old { border-color:#d9822b; color:#d9822b; }
.sig { margin:.2em 0 .4em; }
.cards { display:grid; grid-template-columns:repeat(auto-fill, minmax(260px, 1fr)); gap:14px; margin:1.4em 0; }
.card { display:block; border:1px solid var(--line); border-radius:8px; padding:12px 16px; text-decoration:none; color:var(--fg); background:var(--nav); }
.card:hover { border-color:var(--accent); }
.card b { color:var(--accent); display:block; }
.pdfs { margin-top:2em; font-size:14px; }
@media (max-width: 800px) { .layout { display:block; } .sidebar { position:static; width:auto; height:auto; } main { padding:18px; } }
"

// ---- helpers shared by chapters --------------------------------------------

#let is-html() = target() == "html"

// a link to another chapter: a page link in the HTML, plain text in a PDF
#let ch(slug, body, anchor: none) = context {
  if target() == "html" {
    let href = slug + ".html" + if anchor != none { "#" + anchor } else { "" }
    link(href, body)
  } else {
    body
  }
}

#let note(kind: "note", body) = context {
  let label = (note: "Note", warning: "Warning", tip: "Tip").at(kind)
  if target() == "html" {
    html.elem("div", attrs: (class: "callout " + kind))[
      #html.elem("span", attrs: (class: "label"))[#label]
      #body
    ]
  } else {
    let col = (note: accent, warning: rgb("#d9822b"), tip: rgb("#2f8f5b")).at(kind)
    block(
      width: 100%, inset: (x: 10pt, y: 7pt), fill: rgb("#f5f1ea"),
      stroke: (left: 2.5pt + col), breakable: true,
      [#text(weight: "bold", size: 8pt, fill: col)[#upper(label)] #h(4pt) #body],
    )
  }
}

#let wide-table(columns, ..args) = context {
  if target() == "html" {
    html.elem("div", attrs: (class: "tablewrap"))[#table(columns: columns, ..args)]
  } else {
    table(
      columns: columns, inset: 4pt, stroke: 0.4pt + rgb("#cfc8bd"),
      fill: (_, y) => if y == 0 { rgb("#efe9df") },
      ..args,
    )
  }
}

#let config-table(rows) = {
  let cell = (key, ty, default, meaning, prof, env) => (
    [#key \ #text(size: 0.8em, fill: muted)[#ty]],
    [#default],
    [#meaning #if env != "-" [ \ #text(size: 0.85em, fill: muted)[Env: #env]]],
    [#prof],
  )
  wide-table(
    (auto, auto, 1fr, auto),
    table.header([*Key*], [*Default*], [*Meaning*], [*Profile*]),
    ..rows.map(r => cell(..r)).flatten(),
  )
}

#let env-table(rows) = wide-table(
  (auto, 1fr),
  table.header([*Variable*], [*Effect*]),
  ..rows.map(r => (r.at(0), r.at(1))).flatten(),
)

#let cmd-table(rows) = wide-table(
  (auto, auto, 1fr),
  table.header([*Command*], [*Who*], [*What it does*]),
  ..rows.map(r => (r.at(0), r.at(1), r.at(2))).flatten(),
)

// a field or key list: (name, type, description)
#let fields(rows) = wide-table(
  (auto, auto, 1fr),
  table.header([*Name*], [*Type*], [*Meaning*]),
  ..rows.map(r => (raw(r.at(0)), r.at(1), r.at(2))).flatten(),
)

// ---- the reference entries ----------------------------------------------------

// One entry of the Lua reference. `name` is the exact name the coverage
// check looks for ("game.notice", "p:kick", "on_hit"); `sig` the signature.
#let entry(name, sig, profile: "both", queued: false, optin: none, body) = {
  let ok = name in verified
  context {
    if target() == "html" {
      html.elem("span", attrs: (id: "api-" + name))[]
    }
  }
  heading(level: 4, raw(name))
  [#metadata(name) <api-entry>]
  context {
    let chips = ()
    let chip = (txt, kind) => if target() == "html" {
      html.elem("span", attrs: (class: "chip " + kind))[#txt]
    } else {
      box(inset: (x: 4pt, y: 1.5pt), radius: 3pt, stroke: 0.4pt + muted, text(size: 7.5pt)[#txt])
    }
    let profile-txt = (both: "both profiles", "1.0": "protocol 1.0 only", "0.75": "0.75 profile only").at(profile)
    chips.push(chip(profile-txt, ""))
    if queued { chips.push(chip("queued to the next plugin tick", "old")) }
    if optin != none { chips.push(chip("opt-in: " + optin, "old")) }
    chips.push(if ok { chip("verified by running", "ok") } else { chip("from the C code", "") })
    if target() == "html" {
      html.elem("div", attrs: (class: "meta"))[#chips.join([ ])]
    } else {
      block(above: 0pt, below: 5pt, chips.join(h(3pt)))
    }
  }
  block(above: 0.4em, below: 0.4em, raw(sig, lang: "lua", block: false))
  body
}

// ---- the pages ----------------------------------------------------------------

#let nav(current) = {
  let last = none
  for c in chapters {
    if c.part != last {
      html.elem("div", attrs: (class: "part"))[#c.part]
      last = c.part
    }
    html.elem(
      "a",
      attrs: (href: c.file + ".html", class: if c.file == current { "current" } else { "" }),
    )[#c.title]
  }
}

// HTML page: sidebar + main
#let html-page(slug, title, body) = {
  html.elem("style")[#css]
  html.elem("div", attrs: (class: "layout"))[
    #html.elem("nav", attrs: (class: "sidebar"))[
      #html.elem("a", attrs: (href: "index.html", class: "brand"))[ngspades-server manual]
      #nav(slug)
      #html.elem("div", attrs: (class: "part"))[Downloads]
      #html.elem("a", attrs: (href: "ngspades-server-manual.pdf"))[Full manual (PDF)]
      #if slug != "index" { html.elem("a", attrs: (href: "pdf/" + slug + ".pdf"))[This chapter (PDF)] }
    ]
    #html.elem("main")[
      #html.elem("h1")[#title]
      #body
    ]
  ]
}

#let paged-page(title, body) = {
  set page(paper: "a4", margin: (x: 2cm, y: 2.2cm), numbering: "1",
    header: context if counter(page).get().first() > 1 { text(size: 8pt, fill: muted)[ngspades-server manual #h(1fr) #title] })
  set text(size: 10pt, fill: ink)
  set par(justify: false, leading: 0.62em)
  show heading.where(level: 1): it => { pagebreak(weak: true); text(size: 20pt, fill: accent, weight: "bold", it.body); v(0.5em) }
  show heading.where(level: 2): it => { v(0.8em); text(size: 14pt, weight: "bold", it.body); v(0.2em) }
  show heading.where(level: 3): it => { v(0.6em); text(size: 11.5pt, weight: "bold", it.body); v(0.1em) }
  show heading.where(level: 4): it => { v(0.9em); line(length: 100%, stroke: 0.4pt + rgb("#cfc8bd")); text(size: 10.5pt, weight: "bold", it.body) }
  show raw.where(block: true): it => block(width: 100%, fill: code-bg, inset: 7pt, radius: 3pt, text(size: 8.3pt, it))
  show raw.where(block: false): set text(size: 0.9em)
  show link: set text(fill: accent)
  body
}

// One chapter on its own (HTML or PDF)
#let chapter-page(slug, title, body) = {
  set document(title: title + " - ngspades-server manual")
  context if target() == "html" { html-page(slug, title, body) } else { paged-page(title, { heading(level: 1)[#title]; body }) }
}
