// The connection to Stream Deck: a WebSocket (WinHTTP) on localhost. Stream
// Deck sends key events; we send the key's image.
//
// One thread receives events; a worker polls UsageOne and redraws the key when
// it changes (the countdowns change it every minute). A tap refreshes the
// numbers; holding the key opens Claude.

#include "plugin.h"

#include <winhttp.h>

#include <chrono>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <set>
#include <thread>

namespace Plugin {

namespace {

const auto kPoll = std::chrono::seconds(5);
const auto kPollBusy = std::chrono::seconds(1);  // while UsageOne reads new numbers
const DWORD kLongPressMs = 500;

class Connection {
public:
    bool Connect(int port);
    void Run(const std::string& uuid, const std::string& registerEvent);

private:
    void Send(const std::string& json);
    void OnEvent(const std::string& msg);
    void OnKeyDown(const std::string& context);
    void OnKeyUp();
    void Wake(bool refresh);
    void SetImage(const std::string& context, const std::string& image);
    void ShowOnAllKeys(const std::string& image, bool alert);
    void Worker();
    Status Refresh(bool* failed);

    HINTERNET session_ = nullptr, connect_ = nullptr, socket_ = nullptr;
    std::mutex sendLock_;

    std::mutex lock_;  // everything below
    std::condition_variable wakeUp_;
    bool wake_ = false, refresh_ = false;
    std::set<std::string> contexts_;  // keys showing this action
    std::string lastImage_;
    int press_ = 0;                          // counts presses, to tell a hold from the next press
    bool held_ = false, longFired_ = false;  // the key is down / this press already opened Claude
};

bool Connection::Connect(int port) {
    session_ = WinHttpOpen(L"UsageOne", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    connect_ = session_ ? WinHttpConnect(session_, L"127.0.0.1", (INTERNET_PORT)port, 0) : nullptr;
    HINTERNET request = connect_ ? WinHttpOpenRequest(connect_, L"GET", L"/", nullptr, WINHTTP_NO_REFERER,
                                                      WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
                                 : nullptr;
    if (!request) return false;
    bool ok = WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
              WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr);
    if (ok) socket_ = WinHttpWebSocketCompleteUpgrade(request, 0);
    WinHttpCloseHandle(request);
    return socket_ != nullptr;
}

void Connection::Send(const std::string& json) {
    std::lock_guard<std::mutex> lock(sendLock_);
    if (socket_)
        WinHttpWebSocketSend(socket_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)json.data(), (DWORD)json.size());
}

void Connection::Run(const std::string& uuid, const std::string& registerEvent) {
    Send("{\"event\":\"" + registerEvent + "\",\"uuid\":\"" + uuid + "\"}");
    std::thread(&Connection::Worker, this).detach();

    std::string msg;
    std::vector<char> buf(64 * 1024);
    for (;;) {
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        if (WinHttpWebSocketReceive(socket_, buf.data(), (DWORD)buf.size(), &read, &type) != NO_ERROR) break;
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
        msg.append(buf.data(), read);
        bool complete = type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                        type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
        if (complete) {
            OnEvent(msg);
            msg.clear();
        }
    }
    Log("connection closed");
}

// --- Events -----------------------------------------------------------------------

void Connection::OnEvent(const std::string& msg) {
    std::string event = JsonValue(msg, "event"), context = JsonValue(msg, "context");
    if (event == "willAppear") {
        std::string image;
        {
            std::lock_guard<std::mutex> lock(lock_);
            contexts_.insert(context);
            image = lastImage_;
        }
        if (!image.empty()) SetImage(context, image);
        Wake(false);
    } else if (event == "willDisappear") {
        std::lock_guard<std::mutex> lock(lock_);
        contexts_.erase(context);
    } else if (event == "keyDown") {
        OnKeyDown(context);
    } else if (event == "keyUp") {
        OnKeyUp();
    } else if (event == "systemDidWakeUp") {
        Wake(false);
    }
}

// Holding the key opens Claude once it has been down for kLongPressMs.
void Connection::OnKeyDown(const std::string& context) {
    int press;
    {
        std::lock_guard<std::mutex> lock(lock_);
        press = ++press_;
        held_ = true;
        longFired_ = false;
    }
    std::thread([this, press, context] {
        Sleep(kLongPressMs);
        {
            std::lock_guard<std::mutex> lock(lock_);
            if (press != press_ || !held_) return;
            longFired_ = true;
        }
        OpenClaude();
        Send("{\"event\":\"showOk\",\"context\":\"" + context + "\"}");
    }).detach();
}

// A tap (released before the long press fired) refreshes.
void Connection::OnKeyUp() {
    bool tap;
    {
        std::lock_guard<std::mutex> lock(lock_);
        held_ = false;
        tap = !longFired_;
    }
    if (tap) Wake(true);
}

void Connection::Wake(bool refresh) {
    {
        std::lock_guard<std::mutex> lock(lock_);
        wake_ = true;
        refresh_ = refresh_ || refresh;
    }
    wakeUp_.notify_one();
}

// --- Images -----------------------------------------------------------------------

void Connection::SetImage(const std::string& context, const std::string& image) {
    Send("{\"event\":\"setImage\",\"context\":\"" + context + "\",\"payload\":{\"image\":\"" + image +
         "\",\"target\":0}}");
}

void Connection::ShowOnAllKeys(const std::string& image, bool alert) {
    std::set<std::string> contexts;
    {
        std::lock_guard<std::mutex> lock(lock_);
        contexts = contexts_;
        lastImage_ = image;
    }
    for (const std::string& c : contexts) {
        SetImage(c, image);
        if (alert) Send("{\"event\":\"showAlert\",\"context\":\"" + c + "\"}");
    }
}

// --- Worker -----------------------------------------------------------------------

// Asks UsageOne for new numbers, starting it first if it isn't running.
Status Connection::Refresh(bool* failed) {
    Status s;
    bool ok = Query("refresh", &s);
    if (!ok && StartUsageOne()) {
        for (int i = 0; i < 30 && !ok; i++) {  // it reads the numbers as it starts
            Sleep(500);
            ok = Query("status", &s);
        }
    }
    if (ok) s.probing = true;  // it starts reading right after answering
    *failed = !ok;
    Log("key tapped: %s", ok ? "refreshing" : "UsageOne not available");
    return s;
}

void Connection::Worker() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    KeyPainter painter;
    if (!painter.Init()) {
        Log("drawing setup failed");
        return;
    }
    std::string shown;
    bool busy = false;
    for (;;) {
        bool refresh;
        {
            std::unique_lock<std::mutex> lock(lock_);
            wakeUp_.wait_for(lock, busy ? kPollBusy : kPoll, [this] { return wake_ || refresh_; });
            refresh = refresh_;
            refresh_ = wake_ = false;
        }
        Status s;
        bool failed = false;
        if (refresh)
            s = Refresh(&failed);
        else
            Query("status", &s);
        busy = s.probing;

        std::string image = painter.Render(s, (long long)time(nullptr));
        if (image.empty()) continue;
        bool firstImage;
        {
            std::lock_guard<std::mutex> lock(lock_);
            firstImage = lastImage_.empty();
        }
        if (image != shown || firstImage || failed) {
            ShowOnAllKeys(image, failed);
            shown = image;
        }
    }
}

}  // namespace

int Run(int port, const std::string& uuid, const std::string& registerEvent) {
    static Connection connection;  // lives until the process ends (the worker uses it)
    if (!connection.Connect(port)) {
        Log("could not connect to Stream Deck on port %d (%lu)", port, GetLastError());
        return 1;
    }
    Log("connected");
    connection.Run(uuid, registerEvent);
    return 0;
}

}  // namespace Plugin
