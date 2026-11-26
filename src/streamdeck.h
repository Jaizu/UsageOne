#pragma once

#include "common.h"

// The Stream Deck plugin built into UsageOne.exe: UsageOne.StreamDeck.exe and
// its manifest are resources (res\app.rc). Settings installs or updates it.

struct PluginState {
    bool streamDeck = false;  // the Stream Deck app is installed
    std::wstring installed;   // installed plugin version ("1.0.0"), empty if not installed
    bool installing = false;
    bool failed = false;      // the last install failed
};

namespace StreamDeck {

// Whether Stream Deck is installed and which plugin version it has.
void ReadState(PluginState& state);

// Writes the plugin to a temp folder and runs its installer, which copies it
// into Stream Deck's plugins and restarts Stream Deck. Blocking: call it from
// a worker thread.
bool InstallPlugin();

}  // namespace StreamDeck
