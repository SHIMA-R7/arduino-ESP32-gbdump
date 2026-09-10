"""
Renders a Markdown file to A4 PDF via Edge's headless print-to-PDF.

    python md2pdf.py ../REPORT.ja.md -o report.pdf

Chosen over a PDF library because the report is Japanese: a browser
already has the fonts and line-breaking rules for it, where ReportLab's
built-in fonts have no CJK glyphs at all. Edge ships with Windows, so
there is nothing extra to install beyond the `markdown` package.
"""

import argparse
import html
import os
import subprocess
import sys
import tempfile
import time

import markdown

EDGE_PATHS = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
]

CSS = """
@page { size: A4; margin: 18mm 16mm; }
body {
  font-family: "Yu Gothic", "Meiryo", "MS Gothic", sans-serif;
  font-size: 10.5pt; line-height: 1.75; color: #111;
}
h1 {
  font-size: 19pt; margin: 0 0 4mm; padding-bottom: 3mm;
  border-bottom: 2px solid #333;
}
h2 {
  font-size: 14pt; margin: 9mm 0 3mm; padding-bottom: 1.5mm;
  border-bottom: 1px solid #bbb; page-break-after: avoid;
}
h3 { font-size: 11.5pt; margin: 6mm 0 2mm; page-break-after: avoid; }
h4 { font-size: 10.5pt; margin: 5mm 0 2mm; page-break-after: avoid; }
p, ul, ol { margin: 0 0 3mm; }
li { margin-bottom: 1mm; }
table {
  border-collapse: collapse; width: 100%; margin: 3mm 0 5mm;
  font-size: 9.5pt; page-break-inside: avoid;
}
th, td { border: 1px solid #aaa; padding: 1.6mm 2.5mm; text-align: left;
         vertical-align: top; }
th { background: #eee; font-weight: bold; }
code {
  font-family: Consolas, "MS Gothic", monospace; font-size: 9pt;
  background: #f2f2f2; padding: 0.4mm 1mm; border-radius: 2px;
}
pre {
  background: #f6f6f6; border: 1px solid #ddd; border-radius: 3px;
  padding: 2.5mm 3mm; overflow-x: auto; page-break-inside: avoid;
}
pre code { background: none; padding: 0; font-size: 8.5pt; line-height: 1.5; }
blockquote {
  margin: 3mm 0; padding: 1mm 4mm; border-left: 3px solid #ccc; color: #444;
}
hr { border: none; border-top: 1px solid #ccc; margin: 7mm 0; }
a { color: #0645ad; text-decoration: none; }
strong { font-weight: bold; }
"""


def find_edge():
    for path in EDGE_PATHS:
        if os.path.isfile(path):
            return path
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="Markdown file")
    ap.add_argument("-o", "--out", help="output PDF (default: alongside the source)")
    ap.add_argument("--keep-html", action="store_true",
                    help="keep the intermediate HTML for inspection")
    args = ap.parse_args()

    edge = find_edge()
    if not edge:
        print("Microsoft Edge not found; can't render.")
        return 1

    src = os.path.abspath(args.source)
    out = os.path.abspath(args.out or os.path.splitext(src)[0] + ".pdf")

    with open(src, encoding="utf-8") as f:
        text = f.read()

    body = markdown.markdown(
        text, extensions=["tables", "fenced_code", "sane_lists", "toc"])
    title = html.escape(os.path.basename(src))
    page = (f"<!doctype html><html lang=\"ja\"><head><meta charset=\"utf-8\">"
            f"<title>{title}</title><style>{CSS}</style></head>"
            f"<body>{body}</body></html>")

    tmp_dir = tempfile.mkdtemp(prefix="md2pdf-")
    html_path = os.path.join(tmp_dir, "page.html")
    with open(html_path, "w", encoding="utf-8") as f:
        f.write(page)

    # A fresh profile keeps this from attaching to a running Edge, which
    # would make the headless call return before printing anything.
    profile = os.path.join(tmp_dir, "profile")
    cmd = [
        edge, "--headless=new", "--disable-gpu", "--no-first-run",
        f"--user-data-dir={profile}",
        "--print-to-pdf-no-header",
        f"--print-to-pdf={out}",
        html_path,
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=180)

    # Edge writes the file after the process returns in some versions.
    for _ in range(20):
        if os.path.isfile(out) and os.path.getsize(out) > 0:
            break
        time.sleep(0.5)

    if not os.path.isfile(out):
        print("Edge did not produce a PDF.")
        print(proc.stdout[-2000:])
        print(proc.stderr[-2000:])
        return 1

    print(f"Wrote {out} ({os.path.getsize(out)} bytes)")
    if args.keep_html:
        print(f"HTML kept at {html_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
