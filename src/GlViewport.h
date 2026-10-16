#pragma once

#include <QOpenGLWidget>

class PdfCanvas;

// GPU-backed viewport for PdfCanvas.
//
// The canvas normally paints its pages with the CPU raster engine into a plain
// QWidget viewport. Swapping that viewport for this widget routes every paint
// through the OpenGL paint engine instead, so scrolling, zooming and ink
// compositing are done by the GPU.
//
// It is installed ONLY when 硬件加速 is switched on AND a real OpenGL driver was
// found (see PdfCanvas and gpuRenderAvailable() in PdfCanvas.cpp); otherwise the
// canvas keeps its plain raster viewport, so an unusable driver can never leave
// the teacher looking at a black page.
//
// The painting itself stays in PdfCanvas::paintForViewport(): this class only
// owns the GL surface, and PdfCanvas::viewportEvent() deliberately lets the
// paint event through to it (see the comment there).
class GlViewport : public QOpenGLWidget
{
public:
    explicit GlViewport(PdfCanvas *canvas, QWidget *parent = nullptr);

protected:
    void paintGL() override;

private:
    PdfCanvas *m_canvas = nullptr;
};
