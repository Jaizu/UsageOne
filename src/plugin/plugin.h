#pragma once

// UsageOne's Stream Deck plugin (UsageOne.StreamDeck.exe): a key with the
// session and weekly bars, drawn like the panel. The numbers come from the
// running UsageOne through its status pipe.
//
//   main.cpp        command line: run as a plugin, /install, /make-images
//   connection.cpp  the WebSocket to Stream Deck, key events, the poll loop
//   keyimage.cpp    drawing the key (Direct2D into a PNG)
//   usageone.cpp    talking to UsageOne (pipe, starting it), opening Claude, the log
//   install.cpp     copying the plugin into Stream Deck's plugins folder

#include "../common.h"

#include <memory>
#include <string>
#include <vector>

namespace Plugin {

// --- usageone.cpp ---

// What UsageOne reports (see PublishStatus in ../statuspipe.cpp).
struct Status {
    bool running = false;   // UsageOne answered
    std::string status;     // none, ok, noClaude, notLoggedIn, noLimits, failed
    bool probing = false;   // reading new numbers right now
    int session = -1;       // percent; -1: not reported
    int week = -1;
    long long sessionReset = 0;  // Unix seconds; 0: the window hasn't started
    long long weekReset = 0;
};

// Sends "status" or "refresh" to UsageOne. False if it isn't running.
bool Query(const char* command, Status* out);
// Starts UsageOne in the background, from where it last ran.
bool StartUsageOne();
// The Claude desktop app (brought forward if open), or claude.ai.
void OpenClaude();
// printf-style line in %LOCALAPPDATA%\UsageOne\StreamDeck.log.
void Log(const char* fmt, ...);
// The value of "key" in a flat JSON object: a string's content or a bare token.
std::string JsonValue(const std::string& json, const char* key);

// --- keyimage.cpp ---

const int kKeySize = 144;  // Stream Deck keys are 72x72, 144x144 on high-DPI models

// The key for `status` at `now` (Unix seconds), as a PNG data URL.
class KeyPainter {
public:
    bool Init();
    std::string Render(const Status& status, long long now);
    // The manifest's images (action, category, plugin icon, default key).
    bool WriteManifestImages(const std::wstring& folder);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// --- install.cpp ---

// Copies this exe and its manifest (stamped with this version) into Stream
// Deck's plugins folder, draws the images and restarts Stream Deck. 0 = ok.
int Install();

// --- connection.cpp ---

// Connects to Stream Deck on `port`, registers and runs until it disconnects.
int Run(int port, const std::string& uuid, const std::string& registerEvent);

}  // namespace Plugin
