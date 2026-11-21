// compact_enc_det direct check: how the detector behaves on short game-like strings
// Build:
//   cl /MT /O2 /EHsc /utf-8 /I third_party\compact_enc_det testced.cpp ^
//      third_party\compact_enc_det\compact_enc_det\compact_enc_det.cc ^
//      third_party\compact_enc_det\compact_enc_det\compact_enc_det_hint_code.cc ^
//      third_party\compact_enc_det\util\encodings\encodings.cc ^
//      third_party\compact_enc_det\util\languages\languages.cc /Fe:testced.exe
#include <stdio.h>
#include <string.h>
#include "compact_enc_det/compact_enc_det.h"
#include "util/encodings/encodings.h"

static void Try(const char* tag, const char* s, int len)
{
    int consumed = 0;
    bool reliable = false;
    Encoding enc = CompactEncDet::DetectEncoding(
        s, len,
        nullptr, nullptr, nullptr,          // url / http charset / meta charset hints
        UNKNOWN_ENCODING,
        UNKNOWN_LANGUAGE,
        CompactEncDet::QUERY_CORPUS,        // short queries: closest to TextDraw
        true,
        &consumed, &reliable);

    printf("%-26s -> %-18s reliable=%d\n", tag, EncodingName(enc), (int)reliable);
}

int main()
{
    const char gbk_long[]  = "\xBB\xB6\xD3\xAD\xC0\xB4\xB5\xBD\xD6\xD0\xB9\xFA\xB7\xFE\xCE\xF1\xC6\xF7";
    const char gbk_short[] = "\xC4\xE3\xBA\xC3";
    const char big5[]      = "\xC5\x77\xAA\xEF\xA8\xD3\xA8\xEC\xA4\xA4\xB0\xEA\xA6\xF8\xAA\x41\xBE\xB9";
    const char sjis[]      = "\x93\xFA\x96\x7B\x8C\xEA\x82\xCC\x83\x65\x83\x58\x83\x67\x82\xC5\x82\xB7";
    const char euckr[]     = "\xC7\xD1\xB1\xB9\xBE\xEE\x20\xC5\xD7\xBD\xBA\xC6\xAE\xC0\xD4\xB4\xCF\xB4\xD9";
    const char ru[]        = "\xCF\xF0\xE8\xE2\xE5\xF2\x2C\x20\xEA\xE0\xEA\x20\xE4\xE5\xEB\xE0\x3F";
    const char fr[]        = "Caf\xE9 r\xE9sum\xE9 na\xEFve";
    const char u8[]        = "\xE6\xAC\xA2\xE8\xBF\x8E\xE6\x9D\xA5\xE5\x88\xB0\xE4\xB8\xAD\xE5\x9B\xBD";

    Try("GBK long (18B)",  gbk_long,  sizeof(gbk_long) - 1);
    Try("GBK short (4B)",  gbk_short, sizeof(gbk_short) - 1);
    Try("Big5 long",       big5,      sizeof(big5) - 1);
    Try("Shift-JIS",       sjis,      sizeof(sjis) - 1);
    Try("EUC-KR",          euckr,     sizeof(euckr) - 1);
    Try("CP1251",          ru,        sizeof(ru) - 1);
    Try("CP1252",          fr,        sizeof(fr) - 1);
    Try("UTF-8",           u8,        sizeof(u8) - 1);
    Try("ASCII",           "Hello world", 11);
    return 0;
}
