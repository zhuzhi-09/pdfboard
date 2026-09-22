#pragma once

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

}   // namespace PdfExport
