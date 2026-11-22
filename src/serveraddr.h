// The per-server configuration file's name, and the address it comes from.
//
// SA-MP's client is started as "samp.exe <nick> <host> <port> [password]", so the server a
// player is joining is on the command line before any config is loaded. A file in the
// servers\ folder next to this mod's ini overrides the keys it defines, which is what makes a
// GBK server and a UTF-8 server work without editing the ini back and forth.
//
// The file name is that address, lower cased, with anything outside [a-z0-9._-] written as
// "_" - so 127.0.0.1:7777 becomes 127.0.0.1_7777.ini.
#pragma once

#include <string>
#include <vector>

namespace serveraddr {

inline wchar_t SanitizeChar(wchar_t c)
{
    if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    const bool keep = (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') ||
                      c == L'.' || c == L'_' || c == L'-';
    return keep ? c : L'_';
}

inline std::wstring Sanitize(const std::wstring& text)
{
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) out.push_back(SanitizeChar(text[i]));
    return out;
}

// "127.0.0.1" + "7777" -> "127.0.0.1_7777"; a missing port leaves the host alone.
inline std::wstring FileName(const std::wstring& host, const std::wstring& port)
{
    if (host.empty()) return std::wstring();
    std::wstring name = Sanitize(host);
    if (!port.empty()) {
        name.push_back(L'_');
        name += Sanitize(port);
    }
    return name;
}

// Splits a command line into arguments, honouring double quotes. The first element is the
// executable, as usual.
inline std::vector<std::wstring> SplitArgs(const std::wstring& cmdline)
{
    std::vector<std::wstring> out;
    std::wstring cur;
    bool quoted = false, any = false;
    for (size_t i = 0; i < cmdline.size(); ++i) {
        const wchar_t c = cmdline[i];
        if (c == L'"') { quoted = !quoted; any = true; continue; }
        if (!quoted && (c == L' ' || c == L'\t')) {
            if (any || !cur.empty()) { out.push_back(cur); cur.clear(); any = false; }
            continue;
        }
        cur.push_back(c);
        any = true;
    }
    if (any || !cur.empty()) out.push_back(cur);
    return out;
}

inline bool AllDigits(const std::wstring& s)
{
    if (s.empty()) return false;
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] < L'0' || s[i] > L'9') return false;
    return true;
}

// Pulls the host and port out of the launch line. Two layouts are in use:
//   samp.exe <nick> <host> <port> [password]      positional
//   samp.exe -h <host> -p <port> -n <nick> ...    flag style
// Both are accepted: "-h" wins for the host, and an address-looking token is the fallback.
// The port is "-p <n>" when present, otherwise the token right after a positional host.
// "127.0.0.1:7777" -> host 127.0.0.1, port 7777
inline bool SplitHostPort(const std::wstring& text, std::wstring* host, std::wstring* port)
{
    const size_t colon = text.rfind(L':');
    if (colon == std::wstring::npos || colon == 0 || colon + 1 >= text.size()) return false;
    const std::wstring tail = text.substr(colon + 1);
    if (!AllDigits(tail)) return false;
    if (host) *host = text.substr(0, colon);
    if (port) *port = tail;
    return true;
}

inline bool Parse(const std::wstring& cmdline, std::wstring* host, std::wstring* port)
{
    if (host) host->clear();
    if (port) port->clear();
    const std::vector<std::wstring> args = SplitArgs(cmdline);

    auto flagValue = [&args](const wchar_t* flag) -> const std::wstring* {
        for (size_t i = 1; i + 1 < args.size(); ++i) {
            if (args[i] == flag) return &args[i + 1];
            if (args[i].size() == 2 && args[i][1] == flag[1] &&
                (args[i][0] == L'-' || args[i][0] == L'/'))
                return &args[i + 1];
        }
        return nullptr;
    };

    // -p<digits>, --port=<digits> and --port <digits> in one pass
    auto portFlag = [&args]() -> std::wstring {
        for (size_t i = 1; i < args.size(); ++i) {
            const std::wstring& a = args[i];
            if (a.size() > 2 && (a.compare(0, 2, L"-p") == 0 || a.compare(0, 2, L"/p") == 0) &&
                AllDigits(a.substr(2)))
                return a.substr(2);
            if ((a == L"-p" || a == L"/p") && i + 1 < args.size() && AllDigits(args[i + 1]))
                return args[i + 1];
            if (a == L"--port" && i + 1 < args.size() && AllDigits(args[i + 1])) return args[i + 1];
            if (a.compare(0, 7, L"--port=") == 0 && AllDigits(a.substr(7))) return a.substr(7);
        }
        return std::wstring();
    };

    if (const std::wstring* h = flagValue(L"-h")) {
        std::wstring hh = *h;
        if (SplitHostPort(hh, &hh, port)) { if (host) *host = hh; return true; }
        if (host) *host = hh;
        const std::wstring pf = portFlag();
        if (!pf.empty() && port) *port = pf;
        return !host->empty();
    }

    for (size_t i = 1; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        if (a.empty() || a[0] == L'-' || a[0] == L'/') continue;
        const bool looksHost = a.find(L'.') != std::wstring::npos || a == L"localhost";
        if (!looksHost) continue;
        std::wstring h = a;
        if (SplitHostPort(a, &h, port)) { if (host) *host = h; return true; }
        if (host) *host = a;
        if (port && i + 1 < args.size() && AllDigits(args[i + 1])) *port = args[i + 1];
        else {
            const std::wstring pf = portFlag();
            if (!pf.empty() && port) *port = pf;
        }
        return true;
    }
    return false;
}

} // namespace serveraddr
