"""
Prints a PDF on Windows without needing a PDF reader installed.

    python print_pdf.py report.pdf --printer "Brother DCP-J1270N Printer"
    python print_pdf.py report.pdf --list

Windows has no built-in PDF print path from a script: the shell's PrintTo
verb only works if some application has registered itself for it, and
Edge's headless mode can print *to* a PDF but not *from* one. So this
renders each page to a bitmap with pypdfium2 and sends those through
.NET's PrintDocument, which talks to any installed printer.
"""

import argparse
import os
import subprocess
import sys
import tempfile

import pypdfium2

PS_PRINT = r'''
param([string]$Dir, [string]$Printer, [string]$Paper)

Add-Type -AssemblyName System.Drawing

$files = Get-ChildItem -Path $Dir -Filter page_*.png | Sort-Object Name
if ($files.Count -eq 0) { Write-Error "no rendered pages"; exit 1 }

$doc = New-Object System.Drawing.Printing.PrintDocument
$doc.DocumentName = "PDF print"
if ($Printer) { $doc.PrinterSettings.PrinterName = $Printer }
if (-not $doc.PrinterSettings.IsValid) {
  Write-Error "printer not valid: $Printer"; exit 1
}

foreach ($ps in $doc.PrinterSettings.PaperSizes) {
  if ($ps.PaperName -like "*$Paper*") { $doc.DefaultPageSettings.PaperSize = $ps; break }
}
$doc.DefaultPageSettings.Margins = New-Object System.Drawing.Printing.Margins(0,0,0,0)
$doc.OriginAtMargins = $false

$script:index = 0
$handler = {
  param($sender, $e)
  $img = [System.Drawing.Image]::FromFile($files[$script:index].FullName)
  try {
    # Fit the page image inside the printable area, preserving aspect.
    $area = $e.PageBounds
    $scale = [Math]::Min($area.Width / $img.Width, $area.Height / $img.Height)
    $w = [int]($img.Width * $scale)
    $h = [int]($img.Height * $scale)
    $x = [int](($area.Width - $w) / 2)
    $y = [int](($area.Height - $h) / 2)
    $e.Graphics.DrawImage($img, $x, $y, $w, $h)
  } finally {
    $img.Dispose()
  }
  $script:index++
  $e.HasMorePages = ($script:index -lt $files.Count)
}
$doc.add_PrintPage($handler)
$doc.Print()
Write-Output "sent $($files.Count) page(s) to $($doc.PrinterSettings.PrinterName)"
'''


def list_printers():
    out = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "Get-Printer | Select-Object -ExpandProperty Name"],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    print(out.stdout.strip())


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pdf", nargs="?", help="PDF to print")
    ap.add_argument("--printer", help="printer name (default: system default)")
    ap.add_argument("--paper", default="A4", help="paper size name to match")
    ap.add_argument("--dpi", type=int, default=200, help="render resolution")
    ap.add_argument("--pages", help="page range, e.g. 1-3 (default: all)")
    ap.add_argument("--list", action="store_true", help="list printers and exit")
    args = ap.parse_args()

    if args.list:
        list_printers()
        return 0
    if not args.pdf:
        ap.error("give a PDF, or --list")

    pdf_path = os.path.abspath(args.pdf)
    doc = pypdfium2.PdfDocument(pdf_path)
    total = len(doc)

    if args.pages:
        first, _, last = args.pages.partition("-")
        start = int(first) - 1
        end = int(last) if last else int(first)
    else:
        start, end = 0, total
    end = min(end, total)

    tmp = tempfile.mkdtemp(prefix="printpdf-")
    scale = args.dpi / 72.0
    count = 0
    for i in range(start, end):
        bitmap = doc[i].render(scale=scale)
        bitmap.to_pil().save(os.path.join(tmp, f"page_{i + 1:03d}.png"))
        count += 1
    print(f"Rendered {count} page(s) at {args.dpi} dpi")

    ps_file = os.path.join(tmp, "print.ps1")
    with open(ps_file, "w", encoding="utf-8") as f:
        f.write(PS_PRINT)

    result = subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
         "-File", ps_file, "-Dir", tmp, "-Printer", args.printer or "",
         "-Paper", args.paper],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    print(result.stdout.strip())
    if result.returncode != 0:
        print(result.stderr.strip())
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
