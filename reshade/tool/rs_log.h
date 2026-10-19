// The ReShade tool's own log, s2fm_reshade_tool.log beside the tool's DLL, emptied when the tool first logs in a
// process. One line a call, flushed at once.
#pragma once

void RsToolLog(const char* fmt, ...);
