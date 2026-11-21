// encoding.cpp self test: UTF-8 first, configurable code page, compact_enc_det
// Build:
//   cl /MT /O2 /EHsc /utf-8 /I src /I third_party\compact_enc_det testenc.cpp ^
//      src\encoding.cpp third_party\compact_enc_det\compact_enc_det\compact_enc_det.cc ^
//      third_party\compact_enc_det\compact_enc_det\compact_enc_det_hint_code.cc ^
//      third_party\compact_enc_det\util\encodings\encodings.cc ^
//      third_party\compact_enc_det\util\languages\languages.cc ^
//      /Fe:testenc.exe version.lib user32.lib gdi32.lib
#include "common.h"
#include <cstdio>
#include <string>
#include <cstring>

// Symbols that encoding.cpp expects from config.cpp
Config       g_cfg;
std::wstring g_gameDir, g_pluginStem, g_fontFace;
bool         g_ready = false;
HMODULE      g_sampBase = nullptr;
UINT         g_codePage = 0;
bool         g_utf8First = true;
bool         g_detectCharset = true;

static void Show(const char* tag, const char* bytes, int len, UINT cp, bool utf8, bool detect)
{
    g_codePage = cp;
    g_utf8First = utf8;
    g_detectCharset = detect;
    std::wstring w = ToWide(bytes, len);
    std::string u8 = ToUtf8(w);
    printf("%-34s cp=%-6u utf8=%-2d detect=%-2d -> %s\n",
           tag, cp, (int)utf8, (int)detect, u8.c_str());
}

int main()
{
    printf("system ACP = %u\n\n", GetACP());

    // Real byte samples per language
    const char gbk[]   = "\xBB\xB6\xD3\xAD\xC0\xB4\xB5\xBD\xD6\xD0\xB9\xFA";             // GBK     welcome to China
    const char big5[]  = "\xC5\x77\xAA\xEF\xA8\xD3\xA8\xEC\xA4\xA4\xB0\xEA\xA6\xF8\xAA\x41\xBE\xB9"; // Big5
    const char sjis[]  = "\x93\xFA\x96\x7B\x8C\xEA\x82\xCC\x83\x65\x83\x58\x83\x67\x82\xC5\x82\xB7"; // Shift-JIS
    const char euckr[] = "\xC7\xD1\xB1\xB9\xBE\xEE\x20\xC5\xD7\xBD\xBA\xC6\xAE\xC0\xD4\xB4\xCF\xB4\xD9"; // EUC-KR
    const char ru[]    = "\xCF\xF0\xE8\xE2\xE5\xF2\x2C\x20\xEA\xE0\xEA\x20\xE4\xE5\xEB\xE0\x3F";         // CP1251
    const char u8[]    = "\xE6\xAC\xA2\xE8\xBF\x8E\xE6\x9D\xA5\xE5\x88\xB0\xE4\xB8\xAD\xE5\x9B\xBD";   // UTF-8

    printf("--- normal case: server encoding == system ACP (GBK) ---\n");
    Show("GBK   bytes, auto",      gbk,   (int)strlen(gbk),   0, true, true);
    Show("GBK   bytes, auto,noDet",gbk,   (int)strlen(gbk),   0, true, false);

    printf("\n--- key case: server encoding != system ACP (ACP=936 here) ---\n");
    Show("BIG5  bytes, auto,detect=0",   big5,  (int)strlen(big5),  0, true, false);
    Show("BIG5  bytes, auto,detect=1",   big5,  (int)strlen(big5),  0, true, true);
    Show("SJIS  bytes, auto,detect=0",   sjis,  (int)strlen(sjis),  0, true, false);
    Show("SJIS  bytes, auto,detect=1",   sjis,  (int)strlen(sjis),  0, true, true);
    Show("EUCKR bytes, auto,detect=0",   euckr, (int)strlen(euckr), 0, true, false);
    Show("EUCKR bytes, auto,detect=1",   euckr, (int)strlen(euckr), 0, true, true);
    Show("CP1251 bytes, auto,detect=0",  ru,    (int)strlen(ru),    0, true, false);
    Show("CP1251 bytes, auto,detect=1",  ru,    (int)strlen(ru),    0, true, true);

    printf("\n--- UTF-8 emoji (4-byte sequences) ---\n");
    {
        const char emoji[] = "\xF0\x9F\x98\x80\xF0\x9F\x91\x8D";   // U+1F600 U+1F44D
        g_codePage = 0; g_utf8First = true; g_detectCharset = true;
        std::wstring w = ToWide(emoji, (int)sizeof(emoji) - 1);
        printf("decoded to %d UTF-16 code units:", (int)w.size());
        for (size_t i = 0; i < w.size(); i++) printf(" %04X", (unsigned)(unsigned short)w[i]);
        printf("\n");
    }

    printf("\n--- UTF-8 / forced code page ---\n");
    Show("UTF-8 bytes, auto",             u8,    (int)strlen(u8),    0, true, true);
    Show("UTF-8 bytes, force 936,noUtf8", u8,    (int)strlen(u8),    936, false, true);
    Show("GBK   bytes, force 1251",       gbk,   (int)strlen(gbk),   1251, true, true);
    Show("GBK   bytes, force 936",        gbk,   (int)strlen(gbk),   936, true, true);
    return 0;
}
