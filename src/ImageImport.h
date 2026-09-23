#pragma once

#include <QSize>
#include <QSizeF>
#include <QString>

// Import an image file as a document.
//
// An image has no pages, so it is wrapped into a ONE-PAGE PDF at its own pixel
// size and everything downstream (zoom, ink, .dpz) then works unchanged: the
// page keeps the image's aspect ratio, and the page size in points is
// pixels * 72/96, so rendering the page at 96 dpi returns exactly the original
// bitmap. The embedded samples are lossless - raw DeviceRGB under
// /FlateDecode - and alpha is flattened onto opaque white (a page is paper).
namespace ImageImport {

// True for formats we can decode and offer in the open dialog.
bool isImage(const QString &path);

// Name-filter fragment for the open dialog, e.g.
// "图片 (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff)".
QString dialogFilter();

// Decode `imagePath` and write a ONE-PAGE PDF to `pdfOutPath` (the page keeps
// the image's aspect ratio). Images above kMaxPixels are scaled down (aspect
// kept) and the scale factor is logged through AppLog. Returns false and fills
// `errorOut` on failure; never leaves a partial file.
bool toPdf(const QString &imagePath, const QString &pdfOutPath,
           QString *errorOut = nullptr);

// Convenience overload for the document-open path: picks a stable PDF under the
// temp directory, writes it and returns that path through `pdfOutPath`. This is
// additive; the signature above is the canonical one and does not change.
bool toPdf(const QString &imagePath, QString *pdfOutPath,
           QString *errorOut = nullptr);

// Page size in points for a decoded image: pixels * 72/96.
QSizeF pageSizePtFor(const QSize &pixels);

constexpr int kMaxPixels = 12 * 1000 * 1000;   // ~12 MPx (4000x3000)

}   // namespace ImageImport
