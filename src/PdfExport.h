#pragma once

#include <QImage>
#include <QString>

class PdfCanvas;

// "Inject into PDF": flattens a document + its ink into a brand new, plain
// PDF that any reader can open.
//
// PDFium (through Qt's read-only QPdfDocument wrapper) cannot carry ink
// annotations, so the pages are rasterised, the ink is composited on top at
// the same normalized positions, JPEG encoded and wrapped in a minimal,
// hand-built PDF with a real cross-reference table. No external library is
// involved.
namespace PdfExport {

// `dpi` drives the raster resolution (150 dpi by default). Pages are clamped so
// a single sheet never exceeds ~40 MPx. Returns false with a readable message
// on any I/O or render failure; a failed export never leaves a file behind.
bool exportFlattened(const PdfCanvas *canvas, const QString &outPath,
                     QString *errorOut = nullptr, int dpi = 150);

// Renders ONE page plus its ink at `dpi` and returns the composited raster
// (opaque RGB, the paper is white). The ink is drawn at the same normalized
// positions the on-screen overlay uses; the raster is clamped so a single sheet
// never exceeds ~40 MPx. Returns a null image with `errorOut` set on failure.
// This is the shared per-page helper `exportFlattened` wraps.
QImage renderPageWithInk(const PdfCanvas *canvas, int page, int dpi,
                         QString *errorOut = nullptr);

// Writes every page as a PNG next to `basePath`: `<base without extension>-<n>.png`
// (1-based, zero padded to the width of the page count). Returns the number of
// files written, or -1 with `errorOut` set. A failure removes every file this
// call created, so no partial export is left behind.
int exportPngPages(const PdfCanvas *canvas, const QString &basePath,
                   QString *errorOut = nullptr, int dpi = 96);

// Pure naming rule so it can be asserted without touching the disk:
// ("C:/a/名", 3, 12) -> "C:/a/名-03.png", ("C:/a/名", 3, 5) -> "C:/a/名-3.png",
// ("C:/a/名", 1, 1) -> "C:/a/名-1.png". A trailing extension on `basePath` is
// dropped first.
QString pngPageName(const QString &basePath, int index, int pageCount);

}   // namespace PdfExport
