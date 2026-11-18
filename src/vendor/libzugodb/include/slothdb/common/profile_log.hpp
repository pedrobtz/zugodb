#pragma once
// zuslothdb: the SLOTH_PROFILE timings upstream writes to stderr are
// discarded. A package must not write to the process's stderr (CRAN's
// compiled-code check), and nothing reads them. The arguments are still
// evaluated, so the callers' timing variables stay used.
namespace zugodb {
inline void ProfileLog(const char *, ...) {}
} // namespace zugodb
