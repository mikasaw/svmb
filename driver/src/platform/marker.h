// r141: death-proof progress markers (see marker.cpp). PASSIVE_LEVEL only.
#pragma once

namespace svmb {

void TraceMarker(const char* tag);
// formatted variant for multi-value lines (VMCB PAs etc.), 200-char budget
void TraceMarkerFmt(const char* fmt, ...);

} // namespace svmb
