#pragma once

#include <QString>

// Crash reporting, so an intermittent crash leaves evidence behind.
//
// Why this exists: "dragging the zoom slider sometimes crashes, on one machine only"
// is impossible to fix from a description. This writes three things the moment the
// process dies:
//
//   1. a text log  - exception code, faulting address, and the module + offset
//                    (e.g. "pdfboard.exe+0x0001a2b3"), so the fault can be mapped
//                    back to a function;
//   2. a minidump  - a .dmp next to the log, small and openable in a debugger;
//   3. breadcrumbs - the last few operations the app performed (zoom value, file,
//                    update step...), which is usually what identifies the trigger.
//
// The handler must be async-signal-safe-ish: no Qt calls, no allocation, no
// exceptions. Breadcrumbs are copied into fixed buffers at call time for exactly
// that reason, so the crash path only writes bytes it already owns.
namespace CrashLog {

// Installs the handlers (unhandled exception, std::terminate, qFatal). Call it once
// the application name is set - before any window exists - so the crash directory
// resolves to .../PDFBoard/logs. Idempotent.
void install();

// Records what the app is about to do. Cheap: a bounded memcpy into a ring buffer.
void breadcrumb(const char *what, const QString &detail = QString());

// Directory the crash files go into (created on demand). Empty only if the system
// has no writable app-data location at all.
QString directory();

// Test hook: how many breadcrumbs are currently stored (never more than the ring).
int testBreadcrumbCount();

}   // namespace CrashLog
