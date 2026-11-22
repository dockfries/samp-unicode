// Offline check of the per-server config file naming. No game needed.

#include "../src/serveraddr.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const wchar_t* name, const std::wstring& got, const std::wstring& want)
{
    if (ok) { std::printf("    ok    %ls\n", name); return; }
    ++g_failures;
    std::printf("    FAIL  %ls  got '%ls' want '%ls'\n", name, got.c_str(), want.c_str());
}

void NameCase(const wchar_t* name, const wchar_t* host, const wchar_t* port, const wchar_t* want)
{
    const std::wstring got = serveraddr::FileName(host, port);
    Check(got == want, name, got, want);
}

void ParseCase(const wchar_t* name, const wchar_t* cmd, const wchar_t* wantHost,
               const wchar_t* wantPort, bool wantOk)
{
    std::wstring host, port;
    const bool ok = serveraddr::Parse(cmd, &host, &port);
    const std::wstring h = host, p = port;
    Check(ok == wantOk && h == wantHost && p == wantPort, name, h + L":" + p,
          std::wstring(wantHost) + L":" + wantPort);
}

} // namespace

int main()
{
    std::printf("per-server config file names\n");
    NameCase(L"host and port", L"127.0.0.1", L"7777", L"127.0.0.1_7777");
    NameCase(L"host only", L"127.0.0.1", L"", L"127.0.0.1");
    NameCase(L"lower cased", L"SA-MP.Example.COM", L"7777", L"sa-mp.example.com_7777");
    NameCase(L"colon becomes underscore", L"example.com:7777", L"", L"example.com_7777");
    NameCase(L"spaces and slashes", L"a b/c", L"77", L"a_b_c_77");
    NameCase(L"empty host", L"", L"7777", L"");

    std::printf("\nlaunch line parsing\n");
    ParseCase(L"typical", L"\"C:\\games\\samp.exe\" Player 127.0.0.1 7777 pass",
              L"127.0.0.1", L"7777", true);
    ParseCase(L"localhost", L"samp.exe Player localhost 7777", L"localhost", L"7777", true);
    ParseCase(L"no port", L"samp.exe Player example.com", L"example.com", L"", true);
    ParseCase(L"no address", L"samp.exe Player", L"", L"", false);
    ParseCase(L"quoted path with spaces", L"\"C:\\Program Files\\samp.exe\" Nick a.b 1234",
              L"a.b", L"1234", true);
    ParseCase(L"flag style", L"samp.exe -h 127.0.0.1 -p 7777 -n Player", L"127.0.0.1", L"7777", true);
    ParseCase(L"flag style, no port", L"samp.exe -h example.com -n Player", L"example.com", L"", true);
    ParseCase(L"flag style with /h", L"samp.exe /h 127.0.0.1 /p 7777", L"127.0.0.1", L"7777", true);
    ParseCase(L"address in one token", L"samp.exe -h 127.0.0.1:7777", L"127.0.0.1", L"7777", true);
    ParseCase(L"address in one token, positional", L"samp.exe Nick 127.0.0.1:7777",
              L"127.0.0.1", L"7777", true);
    ParseCase(L"attached port flag", L"samp.exe -h 127.0.0.1 -p7777", L"127.0.0.1", L"7777", true);
    ParseCase(L"long port flag", L"samp.exe -h 127.0.0.1 --port=7777", L"127.0.0.1", L"7777", true);
    ParseCase(L"long port flag, separate", L"samp.exe -h 127.0.0.1 --port 7777",
              L"127.0.0.1", L"7777", true);
    ParseCase(L"positional with -p", L"samp.exe Nick 127.0.0.1 -p 7777", L"127.0.0.1", L"7777", true);

    std::printf("\n%ls (%d failure%ls)\n", g_failures ? L"FAILED" : L"ALL CHECKS PASSED",
                g_failures, g_failures == 1 ? L"" : L"s");
    return g_failures ? 1 : 0;
}
