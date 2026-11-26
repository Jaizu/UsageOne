#pragma once

#include "common.h"
#include "usage.h"

// The pipe the Stream Deck plugin reads the numbers from (STATUS_PIPE_NAME).
// A client writes "status" or "refresh" and a newline and gets one line of
// JSON back:
//   {"status":"ok","probing":false,"plan":"Max 5x","session":91,"sessionReset":1764166140,
//    "week":67,"weekReset":1764280000}
// Resets are Unix seconds (0 = the window hasn't started); a missing limit is -1.
namespace StatusPipe {

// Serves the pipe on its own thread. "refresh" posts WM_APP_REFRESH to `host`.
void Start(HWND host);

// The answer from now on. Call on every state change.
void Publish(const UsageState& state);

}  // namespace StatusPipe
