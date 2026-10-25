#pragma once

// Interactive touch diagnostic window:  pdfboard.exe --probe-touch
//
// Answers one question on the actual classroom panel: does it report a contact
// area? The app's contact-size features (palm/back-of-hand eraser, per-point brush
// width) only activate when Windows sets TOUCH_MASK_CONTACTAREA, so this window
// shows both sides at once:
//   * the raw WM_POINTER touch info (mask bits, rcContact width/height, pressure)
//   * Qt's view of the same contact (QEventPoint::ellipseDiameters / pressure)
// plus the digitizer capabilities Windows reports for the machine.
// Everything is appended to %LOCALAPPDATA%\PDFBoard\logs\touch-probe.txt, so the
// file can be sent back as-is.
int runTouchProbe();
