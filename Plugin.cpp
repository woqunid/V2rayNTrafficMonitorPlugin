#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include "PluginInterface.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {
struct Counters {
    uint64_t proxy_up{};
    uint64_t proxy_down{};
    uint64_t direct_up{};
    uint64_t direct_down{};
};

void SkipWs(std::string_view s, size_t& p, size_t end) {
    while (p < end && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
}

bool ReadString(std::string_view s, size_t& p, size_t end, std::string& out) {
    SkipWs(s, p, end);
    if (p >= end || s[p] != '"') return false;
    ++p; out.clear();
    while (p < end) {
        char c = s[p++];
        if (c == '"') return true;
        if (c == '\\') {
            if (p >= end) return false;
            char e = s[p++];
            if (e == 'u') {
                if (p + 4 > end) return false;
                p += 4; out.push_back('?');
            } else {
                switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: return false;
                }
            }
        } else out.push_back(c);
    }
    return false;
}

size_t Match(std::string_view s, size_t open, char a, char b, size_t end) {
    int depth = 0; bool in_string = false, escaped = false;
    for (size_t i = open; i < end; ++i) {
        char c = s[i];
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') { in_string = true; continue; }
        if (c == a) ++depth;
        else if (c == b && --depth == 0) return i;
    }
    return std::string_view::npos;
}

bool FindObject(std::string_view s, std::string_view key, size_t begin, size_t end, size_t& ob, size_t& oe) {
    std::string needle = "\"" + std::string(key) + "\"";
    size_t p = s.find(needle, begin);
    while (p != std::string_view::npos && p < end) {
        p += needle.size(); SkipWs(s, p, end);
        if (p < end && s[p] == ':') { ++p; SkipWs(s, p, end); }
        if (p < end && s[p] == '{') {
            size_t q = Match(s, p, '{', '}', end);
            if (q != std::string_view::npos) { ob = p; oe = q; return true; }
        }
        p = s.find(needle, p);
    }
    return false;
}

uint64_t ReadUInt(std::string_view s, size_t begin, size_t end, std::string_view key) {
    std::string needle = "\"" + std::string(key) + "\"";
    size_t p = s.find(needle, begin);
    if (p == std::string_view::npos || p >= end) return 0;
    p += needle.size(); SkipWs(s, p, end);
    if (p >= end || s[p++] != ':') return 0;
    SkipWs(s, p, end);
    uint64_t v = 0;
    while (p < end && s[p] >= '0' && s[p] <= '9') {
        unsigned d = unsigned(s[p++] - '0');
        if (v > (std::numeric_limits<uint64_t>::max() - d) / 10) return std::numeric_limits<uint64_t>::max();
        v = v * 10 + d;
    }
    return v;
}

void SatAdd(uint64_t& a, uint64_t b) {
    uint64_t m = std::numeric_limits<uint64_t>::max();
    a = (a > m - b) ? m : a + b;
}

bool ParseVars(const std::string& json, Counters& out) {
    out = {};
    std::string_view s(json);
    size_t sb, se, ob, oe;
    if (!FindObject(s, "stats", 0, s.size(), sb, se)) return false;
    if (!FindObject(s, "outbound", sb + 1, se, ob, oe)) return false;
    size_t p = ob + 1;
    while (p < oe) {
        SkipWs(s, p, oe);
        if (p < oe && s[p] == ',') { ++p; continue; }
        if (p >= oe) break;
        std::string key;
        if (!ReadString(s, p, oe, key)) return false;
        SkipWs(s, p, oe);
        if (p >= oe || s[p++] != ':') return false;
        SkipWs(s, p, oe);
        if (p >= oe) return false;
        if (s[p] != '{') {
            while (p < oe && s[p] != ',') ++p;
            continue;
        }
        size_t q = Match(s, p, '{', '}', oe + 1);
        if (q == std::string_view::npos) return false;
        uint64_t up = ReadUInt(s, p + 1, q, "uplink");
        uint64_t down = ReadUInt(s, p + 1, q, "downlink");
        if (key.rfind("proxy", 0) == 0) {
            SatAdd(out.proxy_up, up); SatAdd(out.proxy_down, down);
        } else if (key == "direct") {
            out.direct_up = up; out.direct_down = down;
        }
        p = q + 1;
    }
    return true;
}

struct WinHttpHandle {
    HINTERNET h{};
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
    operator HINTERNET() const { return h; }
};

bool HttpGetVars(uint16_t port, Counters& counters) {
    WinHttpHandle session{ WinHttpOpen(L"V2rayNTraffic/0.2", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0) };
    if (!session) return false;
    WinHttpSetTimeouts(session, 200, 200, 250, 250);
    WinHttpHandle connect{ WinHttpConnect(session, L"127.0.0.1", port, 0) };
    if (!connect) return false;
    WinHttpHandle req{ WinHttpOpenRequest(connect, L"GET", L"/debug/vars", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) };
    if (!req) return false;
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return false;
    if (!WinHttpReceiveResponse(req, nullptr)) return false;
    DWORD status = 0, len = sizeof(status);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX) || status != 200) return false;
    std::string body;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(req, &available)) return false;
        if (!available) break;
        size_t old = body.size();
        body.resize(old + available);
        DWORD read = 0;
        if (!WinHttpReadData(req, body.data() + old, available, &read)) return false;
        body.resize(old + read);
        if (body.size() > 4 * 1024 * 1024) return false;
    }
    return ParseVars(body, counters);
}

std::unordered_set<DWORD> XrayPids() {
    std::unordered_set<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring n = pe.szExeFile;
            std::transform(n.begin(), n.end(), n.begin(), [](wchar_t c){ return (wchar_t)towlower(c); });
            if (n == L"xray.exe" || n == L"v2ray.exe") pids.insert(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

std::vector<uint16_t> ListeningPortsFor(const std::unordered_set<DWORD>& pids) {
    std::vector<uint16_t> ports;
    if (pids.empty()) return ports;
    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
    if (!size) return ports;
    std::vector<unsigned char> buf(size);
    auto* table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buf.data());
    if (GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) return ports;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& r = table->table[i];
        if (pids.count(r.dwOwningPid)) ports.push_back(ntohs(static_cast<u_short>(r.dwLocalPort)));
    }
    std::sort(ports.begin(), ports.end());
    ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
    return ports;
}

std::wstring FormatSpeed(double bps) {
    if (!std::isfinite(bps) || bps < 0) bps = 0;
    const wchar_t* units[] = { L"B/s", L"KB/s", L"MB/s", L"GB/s" };
    int u = 0; while (bps >= 1024.0 && u < 3) { bps /= 1024.0; ++u; }
    std::wostringstream os;
    os << std::fixed << std::setprecision((u == 0 || bps < 100.0) ? 1 : 0) << bps << L' ' << units[u];
    return os.str();
}
}

struct Snapshot {
    bool connected{};
    uint16_t port{};
    double pu{}, pd{}, du{}, dd{};
    std::wstring status{L"Waiting for Xray metrics"};
};

class Metrics {
public:
    void Update() {
        std::lock_guard<std::mutex> guard(update_mutex_);
        Counters c;
        uint16_t p = port_;
        if (!p || !HttpGetVars(p, c)) {
            p = 0;
            for (uint16_t candidate : ListeningPortsFor(XrayPids())) {
                if (HttpGetVars(candidate, c)) { p = candidate; break; }
            }
            if (!p) { port_ = 0; baseline_ = false; SetDisconnected(); return; }
            port_ = p; baseline_ = false;
        }
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!baseline_) {
            previous_ = c; previous_time_ = now; baseline_ = true;
            snap_ = {}; snap_.connected = true; snap_.port = p; snap_.status = L"Connected; establishing baseline";
            return;
        }
        double seconds = std::chrono::duration<double>(now - previous_time_).count();
        bool reset = c.proxy_up < previous_.proxy_up || c.proxy_down < previous_.proxy_down || c.direct_up < previous_.direct_up || c.direct_down < previous_.direct_down;
        if (reset || seconds <= 0.0) {
            previous_ = c; previous_time_ = now;
            snap_ = {}; snap_.connected = true; snap_.port = p; snap_.status = L"Counters reset; establishing baseline";
            return;
        }
        snap_.connected = true; snap_.port = p; snap_.status = L"Connected to Xray metrics";
        snap_.pu = double(c.proxy_up - previous_.proxy_up) / seconds;
        snap_.pd = double(c.proxy_down - previous_.proxy_down) / seconds;
        snap_.du = double(c.direct_up - previous_.direct_up) / seconds;
        snap_.dd = double(c.direct_down - previous_.direct_down) / seconds;
        previous_ = c; previous_time_ = now;
    }
    Snapshot Get() const { std::lock_guard<std::mutex> lock(mutex_); return snap_; }
private:
    void SetDisconnected() {
        std::lock_guard<std::mutex> lock(mutex_);
        snap_ = {}; snap_.status = L"Xray /debug/vars not found";
    }
    uint16_t port_{}; bool baseline_{}; Counters previous_{};
    std::chrono::steady_clock::time_point previous_time_{};
    std::mutex update_mutex_; mutable std::mutex mutex_; Snapshot snap_{};
};

class Plugin;
class Item final : public IPluginItem {
public:
    explicit Item(Plugin& owner) : owner_(owner) {}
    const wchar_t* GetItemName() const override { return L"v2rayN proxy/direct speed"; }
    const wchar_t* GetItemId() const override { return L"v2rayn_proxy_direct_speed"; }
    const wchar_t* GetItemLableText() const override { return L""; }
    const wchar_t* GetItemValueText() const override { return L""; }
    const wchar_t* GetItemValueSampleText() const override { return L""; }
    bool IsCustomDraw() const override { return true; }
    int GetItemWidth() const override { return 220; }
    bool DrawItemEx(IPluginDrawer* d, int x, int y, int w, int h, bool dark) override;
    int IsDoubleLineExclusive() const override { return 1; }
private:
    Plugin& owner_;
};

class Plugin final : public ITMPlugin {
public:
    static Plugin& Instance() { static Plugin p; return p; }
    IPluginItem* GetItem(int i) override { return i == 0 ? &item_ : nullptr; }
    void DataRequired() override { metrics_.Update(); }
    const wchar_t* GetInfo(PluginInfoIndex i) override {
        switch (i) {
        case TMI_NAME: return L"v2rayN Proxy/Direct Speed";
        case TMI_DESCRIPTION: return L"Shows Xray proxy/direct upload and download speed in TrafficMonitor.";
        case TMI_AUTHOR: return L"woqunid / OpenAI";
        case TMI_VERSION: return L"0.2.0";
        case TMI_URL: return L"https://github.com/woqunid/V2rayNTrafficMonitorPlugin";
        default: return L"";
        }
    }
    const wchar_t* GetTooltipInfo() override {
        thread_local std::wstring t;
        auto s = metrics_.Get(); auto lines = Lines(s);
        t = s.status;
        if (s.connected) t += L"\n127.0.0.1:" + std::to_wstring(s.port);
        t += L"\n" + lines.first + L"\n" + lines.second;
        return t.c_str();
    }
    void OnExtenedInfo(ExtendedInfoIndex i, const wchar_t* data) override {
        if (i == EI_VALUE_TEXT_COLOR && data) {
            wchar_t* end{}; unsigned long v = wcstoul(data, &end, 10);
            if (end != data) { color_.store(v); have_color_.store(true); }
        }
    }
    std::pair<std::wstring,std::wstring> Lines(const Snapshot& s) const {
        if (!s.connected) return {L"proxy : --", L"direct: --"};
        return {L"proxy : " + FormatSpeed(s.pu) + L"↑ | " + FormatSpeed(s.pd) + L"↓",
                L"direct: " + FormatSpeed(s.du) + L"↑ | " + FormatSpeed(s.dd) + L"↓"};
    }
    std::pair<std::wstring,std::wstring> Lines() const { return Lines(metrics_.Get()); }
    unsigned long Color(bool dark) const { return have_color_.load() ? color_.load() : (dark ? RGB(255,255,255) : RGB(0,0,0)); }
private:
    Plugin() : item_(*this) {}
    Metrics metrics_; Item item_; std::atomic<unsigned long> color_{0}; std::atomic<bool> have_color_{false};
};

bool Item::DrawItemEx(IPluginDrawer* d, int x, int y, int w, int h, bool dark) {
    if (!d) return false;
    auto lines = owner_.Lines(); int half = std::max(1, h / 2); unsigned long c = owner_.Color(dark);
    d->DrawWindowText(x + 2, y, std::max(1,w - 4), half, lines.first.c_str(), c, IPluginDrawer::LEFT, false);
    d->DrawWindowText(x + 2, y + half, std::max(1,w - 4), h - half, lines.second.c_str(), c, IPluginDrawer::LEFT, false);
    return true;
}

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance() { return &Plugin::Instance(); }
