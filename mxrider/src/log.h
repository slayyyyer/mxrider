// log.h
//   Tiny thread-safe text log (mxrider.log next to the DLL).
#pragma once

namespace mxrider {

// Opens (or truncates) the log file. Returns false when the path cannot be written, which lets
// the caller pick a writable location instead of running with no diagnostics at all.
bool LogOpen(const wchar_t* path);
void LogClose();

// printf-style. Each call writes one timestamped line.
void Log(const char* format, ...);

// Folder of the log file (with a trailing backslash), for other output files.
const wchar_t* LogDirectory();

}  // namespace mxrider
