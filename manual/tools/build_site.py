#!/usr/bin/env python3
"""Build site/: HTML for every chapter and the landing page, with the shared
stylesheet that template.typ emits moved into site/style.css."""
import re, subprocess, sys, pathlib, shutil

root = pathlib.Path(__file__).resolve().parents[1]
site = root / "site"
site.mkdir(exist_ok=True)
chapters = re.findall(r'file: "([^"]+)"', (root / "chapters.typ").read_text())

def compile_html(target, args):
    out = site / (target + ".html")
    r = subprocess.run(["typst", "compile", "--features", "html", "--format", "html", "--root", str(root)] + args + [str(out)],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"typst failed for {target}:\n{r.stderr}")
    return out

css = None
for name in chapters + ["index"]:
    if name == "index":
        out = compile_html("index", [str(root / "index.typ")])
    else:
        out = compile_html(name, ["--input", f"chapter={name}", str(root / "entry.typ")])
    h = out.read_text()
    m = re.search(r"<style>(.*?)</style>", h, re.S)
    if m:
        css = css or m.group(1)
        h = h.replace(m.group(0), "")
        h = h.replace("</head>", '<link rel="stylesheet" href="style.css"></head>', 1)
    h = h.replace(" class>", ">")
    out.write_text(h)
    print("html", out.name)
# the CSS arrives with HTML entities for some characters
import html as _html
(site / "style.css").write_text(_html.unescape(css) + "\n")
(site / ".nojekyll").write_text("")
