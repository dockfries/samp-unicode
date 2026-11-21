// ---------------------------------------------------------------------------
// encoding.cpp - text encoding layer
//
// Pipeline for a raw byte string coming from the server:
//   1) pure ASCII            -> widen directly (zero cost)
//   2) Utf8First=1           -> strict UTF-8 (invalid sequences fail)
//   3) CodePage=auto + Detect-> Google compact_enc_det guesses the charset;
//                               only structurally distinctive results are
//                               accepted (CJK / Cyrillic). Latin1 / ASCII
//                               never override the system default.
//   4) fallback              -> MultiByteToWideChar(configured code page),
//                               where "auto" means GetACP()
//
// Why the conversion still uses Windows NLS instead of ICU/iconv:
//   the authoritative tables for CP936 / CP1251 / Big5 ... ship with Windows,
//   and that is exactly what the server and the game used. A third-party copy
//   of the same Unicode mappings buys no accuracy but adds 20-30 MB of
//   dependency and DLL deployment. What genuinely benefits from a library is
//   charset *detection*, so that part is delegated to compact_enc_det.
//
// All results are cached by raw bytes, because the TextDraw / GameText hooks
// are called every frame with the same strings.
// ---------------------------------------------------------------------------
#include "common.h"

// Third-party detector (vendored under third_party/compact_enc_det)
#include "compact_enc_det/compact_enc_det.h"
#include "util/encodings/encodings.h"

#include <cstring>
#include <cstdio>

// defined in config.cpp
extern UINT g_codePage;
extern bool g_utf8First;
extern bool g_detectCharset;
extern int  g_singleBytePolicy;   // 0 system, 1 detect, >1 forced code page

namespace {

constexpr size_t kCacheMax = 512;

CRITICAL_SECTION g_cacheCs;
bool g_cacheCsInit = false;

struct CacheEntry { std::wstring text; };
std::unordered_map<std::string, CacheEntry> g_cache;

void CacheInit()
{
    if (!g_cacheCsInit) { ::InitializeCriticalSection(&g_cacheCs); g_cacheCsInit = true; }
}

// MultiByteToWideChar wrapper; returns false on failure
bool Convert(UINT cp, DWORD flags, const char* s, int len, std::wstring* out)
{
    int n = ::MultiByteToWideChar(cp, flags, s, len, nullptr, 0);
    if (n <= 0) return false;
    out->assign((size_t)n, L'\0');
    int r = ::MultiByteToWideChar(cp, flags, s, len, &(*out)[0], n);
    if (r <= 0) { out->clear(); return false; }
    out->resize((size_t)r);
    return true;
}

// True for the CJK code pages this plugin is usually configured with.
bool IsCjkCodePage(UINT cp)
{
    return cp == 936 || cp == 950 || cp == 932 || cp == 949 || cp == 1361;
}

// Any ideograph or kana in the result. Used only to break ties.
bool ContainsCjk(const std::wstring& s)
{
    for (size_t i = 0; i < s.size(); i++) {
        wchar_t c = s[i];
        if (c >= 0x3000 && c <= 0x30FF) return true;   // CJK punctuation, kana
        if (c >= 0x3400 && c <= 0x4DBF) return true;   // extension A
        if (c >= 0x4E00 && c <= 0x9FFF) return true;   // unified ideographs
        if (c >= 0xAC00 && c <= 0xD7AF) return true;   // hangul
        if (c >= 0xF900 && c <= 0xFAFF) return true;   // compatibility
    }
    return false;
}

// A two-byte legacy character such as GBK D2 BB is also a valid UTF-8 sequence
// (U+04BB), so a short string can pass the strict UTF-8 test and come out as a
// Cyrillic letter. When the legacy reading produces CJK and the UTF-8 reading
// does not, the legacy one is what the server meant.
UINT DetectHardCodePage(const char* s, int len);   // defined below

bool PreferLegacyCjk(UINT cp, const char* s, int len, std::wstring* out)
{
    if (!IsCjkCodePage(cp) || ContainsCjk(*out)) return false;

    // "the UTF-8 reading has no CJK, the code page reading does" is not enough on its
    // own: a short UTF-8 label in Cyrillic or Latin-1 ("Привет", "Olá") is also valid
    // GBK, and swapping turned it into CJK mojibake.
    //
    // But the opposite case is just as real: a menu header like "col " + D2 BB is
    // valid UTF-8 and decodes to "col һ" (a Cyrillic letter that looks like h), while
    // the code page gives "col 一". The detector alone gets that one wrong, so a second
    // path exists: a string that is mostly ASCII, whose only non-ASCII characters are
    // not accented Latin, and which the code page reads as CJK, is CJK text that
    // happens to be valid UTF-8.
    std::wstring legacy;
    if (!Convert(cp, 0, s, len, &legacy)) return false;
    if (!ContainsCjk(legacy)) return false;

    const UINT looks = DetectHardCodePage(s, len);
    if (looks != cp) {
        int ascii = 0, high = 0;
        bool accentedLatin = false;
        for (int i = 0; i < len; i++) {
            const unsigned char c = (unsigned char)s[i];
            if (c < 0x80) ascii++; else high++;
        }
        for (wchar_t c : *out) {
            if (c >= 0x00A0 && c <= 0x024F) { accentedLatin = true; break; }
        }
        if (accentedLatin || ascii <= high) return false;
    }

    out->swap(legacy);
    return true;
}

// Map only the encodings where a wrong guess would be catastrophic and where
// the byte structure is distinctive enough to trust the detector.
UINT MapHardEncoding(Encoding e)
{
    switch (e) {
    case CHINESE_GB:
    case GBK:
        return 936;
    case GB18030:
        return ::IsValidCodePage(54936) ? 54936u : 936u;
    case CHINESE_BIG5:
    case CHINESE_BIG5_CP950:
    case BIG5_HKSCS:
        return 950;
    case JAPANESE_SHIFT_JIS:
    case JAPANESE_CP932:
    case KDDI_SHIFT_JIS:
    case DOCOMO_SHIFT_JIS:
    case SOFTBANK_SHIFT_JIS:
        return 932;
    case JAPANESE_EUC_JP:
        return ::IsValidCodePage(51932) ? 51932u : 0u;
    case KOREAN_EUC_KR:
        return 949;
    case RUSSIAN_CP1251:
        return 1251;
    case RUSSIAN_KOI8_R:
        return 20866;
    case RUSSIAN_KOI8_RU:
        return 21866;
    case RUSSIAN_CP866:
        return 866;
    case UTF8:
        return CP_UTF8;
    default:
        return 0;   // Latin1 / ASCII / everything else: keep the system default
    }
}

// Single-byte Windows code pages: Spanish is 1252, Polish 1250, Arabic 1256, and a server that
// sends them sends the *ANSI* bytes of the machine it was built on, not UTF-8. These are kept
// out of MapHardEncoding because a wrong guess on a short string is destructive, so they are
// only used when [encoding] single_byte asks for it.
UINT MapSingleByteEncoding(Encoding e)
{
    switch (e) {
    case MSFT_CP1250: return 1250;   // Polish, Czech, Hungarian
    case RUSSIAN_CP1251: return 1251;
    case MSFT_CP1252: return 1252;   // Spanish, French, German
    case MSFT_CP1253: return 1253;   // Greek
    case MSFT_CP1254: return 1254;   // Turkish
    case MSFT_CP1255: return 1255;   // Hebrew
    case MSFT_CP1256: return 1256;   // Arabic
    case MSFT_CP1257: return 1257;   // Baltic
    // The sender is a Windows server, so its \"ANSI\" code page is the Windows-125x one. When
    // the detector names the ISO-8859 sibling instead - it does exactly that for Polish, calling
    // CP1250 bytes \"Latin2\" - the Windows page is the one that decodes the bytes correctly.
    case ISO_8859_1:  return 1252;
    case ISO_8859_2:  return 1250;   // Polish, Czech, Hungarian
    case ISO_8859_5:  return 1251;   // Cyrillic
    case ISO_8859_6:  return 1256;   // Arabic
    case ISO_8859_7:  return 1253;   // Greek
    case ISO_8859_9:  return 1254;   // Turkish
    case ISO_8859_15: return 1252;   // Western Europe with the euro sign
    default: return 0;
    }
}

// The detector's single-byte answer for a message that is not UTF-8 and not CJK.
UINT DetectSingleByteCodePage(const char* s, int len)
{
    int consumed = 0;
    bool reliable = false;
    const Encoding e = CompactEncDet::DetectEncoding(
        s, len, nullptr, nullptr, nullptr, UNKNOWN_ENCODING, UNKNOWN_LANGUAGE,
        CompactEncDet::QUERY_CORPUS, true, &consumed, &reliable);
    UINT cp = MapSingleByteEncoding(e);
    if (cp && !::IsValidCodePage(cp)) cp = 0;
    return cp;
}

UINT DetectHardCodePage(const char* s, int len)
{
    int consumed = 0;
    bool reliable = false;
    Encoding e = CompactEncDet::DetectEncoding(
        s, len,
        nullptr, nullptr, nullptr,          // no url / http / meta hints
        UNKNOWN_ENCODING,
        UNKNOWN_LANGUAGE,
        CompactEncDet::QUERY_CORPUS,        // short queries: closest to TextDraw
        true,                               // ignore 7-bit mail encodings
        &consumed,
        &reliable);
    (void)reliable;

    UINT cp = MapHardEncoding(e);
    if (cp && !::IsValidCodePage(cp)) cp = 0;
    return cp;
}

} // namespace

// ---------------------------------------------------------------------------
UINT ResolveCodePage()
{
    UINT cp = g_codePage ? (UINT)g_codePage : ::GetACP();
    if (!cp || !::IsValidCodePage(cp)) cp = ::GetACP();
    return cp;
}

std::wstring CodePageName(UINT cp)
{
    // Use our own English names first: GetCPInfoExW returns a localized name
    // (e.g. the Chinese "GBK" label), and the log must stay English everywhere.
    switch (cp) {
    case 936:   return L"GBK (Simplified Chinese)";
    case 950:   return L"Big5 (Traditional Chinese)";
    case 932:   return L"Shift-JIS (Japanese)";
    case 949:   return L"EUC-KR (Korean)";
    case 874:   return L"TIS-620 (Thai)";
    case 866:   return L"CP866 (DOS Cyrillic)";
    case 1250:  return L"CP1250 (Central Europe)";
    case 1251:  return L"CP1251 (Cyrillic)";
    case 1252:  return L"CP1252 (Western Europe)";
    case 1253:  return L"CP1253 (Greek)";
    case 1254:  return L"CP1254 (Turkish)";
    case 1255:  return L"CP1255 (Hebrew)";
    case 1256:  return L"CP1256 (Arabic)";
    case 1257:  return L"CP1257 (Baltic)";
    case 1258:  return L"CP1258 (Vietnamese)";
    case 54936: return L"GB18030";
    case 20866: return L"KOI8-R";
    case 21866: return L"KOI8-U";
    case 20127: return L"US-ASCII";
    case 65001: return L"UTF-8";
    case 28591: return L"ISO-8859-1";
    case 28592: return L"ISO-8859-2";
    case 28595: return L"ISO-8859-5";
    case 28597: return L"ISO-8859-7";
    case 28599: return L"ISO-8859-9";
    case 28605: return L"ISO-8859-15";
    default: break;
    }

    CPINFOEXW info = {};
    if (::GetCPInfoExW(cp, 0, &info)) return std::wstring(info.CodePageName);
    return L"unknown";
}

// Glyph probe samples per code page (kept as real script samples on purpose)
const wchar_t* ProbeStringForCp(UINT cp)
{
    switch (cp) {
    case 936:  return L"\u4E2D\u6587\u6D4B\u8BD5\u6B66\u4FA0";   // GBK
    case 950:  return L"\u4E2D\u6587\u6E2C\u8A66\u6B66\u4FE0";   // Big5
    case 932:  return L"\u65E5\u672C\u8A9E\u30C6\u30B9\u30C8";   // Shift-JIS
    case 949:  return L"\uD55C\uAD6D\uC5B4\uD14C\uC2A4\uD2B8";   // EUC-KR
    case 874:  return L"\u0E01\u0E02\u0E04\u0E07\u0E08\u0E09";   // Thai
    case 1250: return L"\u00C1\u010C\u010E\u00C9\u011A\u00CD";   // Central Europe
    case 1251: return L"\u0410\u0411\u0412\u0413\u0414\u0415\u0416"; // Cyrillic
    case 866:  return L"\u0410\u0411\u0412\u0413\u0414\u0415\u0416"; // DOS Cyrillic
    case 1253: return L"\u0391\u0392\u0393\u0394\u0395\u0396";   // Greek
    case 1254: return L"\u011E\u0130\u015E\u00C7\u00D6\u00DC";   // Turkish
    case 1255: return L"\u05D0\u05D1\u05D2\u05D3\u05D4\u05D5";   // Hebrew
    case 1256: return L"\u0627\u0628\u062A\u062B\u062C\u062D";   // Arabic
    case 1257: return L"\u0104\u010C\u0118\u0116\u012E\u0160";   // Baltic
    case 1258: return L"\u0102\u00C2\u00CA\u00D4\u01A0";         // Vietnamese
    case 1252: return L"\u00C1\u00C9\u00CD\u00D3\u00DA\u00D1";   // Western Europe
    default:   return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------
std::wstring ToWide(const char* s, int len)
{
    if (!s) return std::wstring();
    if (len < 0) len = (int)::strlen(s);
    if (len <= 0) return std::wstring();

    // 1) pure ASCII: widen directly (all ANSI code pages agree on 0x00-0x7F)
    bool ascii = true;
    for (int i = 0; i < len; i++) {
        if ((unsigned char)s[i] >= 0x80) { ascii = false; break; }
    }
    if (ascii) return std::wstring(s, s + len);

    // 2) cache (the mode is part of the identity; it only changes in tests)
    CacheInit();
    ::EnterCriticalSection(&g_cacheCs);
    static UINT s_lastCp = 0xFFFFFFFFu;
    static bool s_lastUtf8 = false, s_lastDetect = false;
    if (s_lastCp != g_codePage || s_lastUtf8 != g_utf8First || s_lastDetect != g_detectCharset) {
        g_cache.clear();
        s_lastCp = g_codePage;
        s_lastUtf8 = g_utf8First;
        s_lastDetect = g_detectCharset;
    }
    std::string key(s, (size_t)len);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) {
        std::wstring hit = it->second.text;
        ::LeaveCriticalSection(&g_cacheCs);
        return hit;
    }
    ::LeaveCriticalSection(&g_cacheCs);

    // 3) decide the code page
    std::wstring out;
    bool done = false;

    if (g_utf8First) {
        // strict mode: invalid sequences fail, so GBK/1251 is not mistaken for UTF-8
        done = Convert(CP_UTF8, MB_ERR_INVALID_CHARS, s, len, &out);
        if (done) {
            // A two-byte GBK sequence can also be valid UTF-8 (D2 BB = U+04BB).
            // With charset = auto there is no code page yet, so ask the detector
            // which legacy code page this looks like before trusting UTF-8.
            UINT legacy = g_codePage;
            if (!legacy && g_detectCharset) {
                const UINT cp = DetectHardCodePage(s, len);
                if (cp && cp != CP_UTF8) legacy = cp;
            }
            PreferLegacyCjk(legacy ? legacy : ::GetACP(), s, len, &out);
        }
    }

    if (!done && g_detectCharset && g_codePage == 0) {
        UINT cp = DetectHardCodePage(s, len);
        if (cp == CP_UTF8)  done = Convert(CP_UTF8, 0, s, len, &out);
        else if (cp)        done = Convert(cp, 0, s, len, &out);
    }

    // A server that sends Spanish, Polish or Arabic bytes sends the ANSI code page of the
    // machine it was built on: 1252, 1250, 1256. Those are single-byte encodings, so every
    // high byte is a letter - there is no byte structure to recognise, and the detector's
    // guess is only worth taking when the caller asked for it.
    if (!done && g_codePage == 0 && g_singleBytePolicy != 0) {
        bool hasHigh = false;
        for (int i = 0; i < len && !hasHigh; i++)
            if ((unsigned char)s[i] >= 0x80) hasHigh = true;
        if (hasHigh) {
            const UINT cp = (g_singleBytePolicy > 1) ? (UINT)g_singleBytePolicy
                                                     : DetectSingleByteCodePage(s, len);
            if (cp && cp != CP_UTF8 && ::IsValidCodePage(cp)) {
                done = Convert(cp, 0, s, len, &out);
                // The guess is worth seeing when it fires: it is the one decision that can turn a
                // message into letters from the wrong alphabet, and it differs per server.
                if (done) {
                    static UINT lastLogged = 0;
                    if (lastLogged != cp) {
                        lastLogged = cp;
                        LogDbg(L"encoding: single-byte message decoded as %u", cp);
                    }
                }
            }
        }
    }

    if (!done) {
        UINT cp = ResolveCodePage();
        if (cp == CP_UTF8) done = Convert(CP_UTF8, 0, s, len, &out);
        else               done = Convert(cp, 0, s, len, &out);
    }

    if (!done) {
        // last resort: never return an empty string
        if (!Convert(::GetACP(), 0, s, len, &out))
            out.assign((size_t)len, L'\uFFFD');
    }

    // 4) store in the cache
    ::EnterCriticalSection(&g_cacheCs);
    if (g_cache.size() >= kCacheMax) g_cache.clear();
    g_cache.emplace(std::move(key), CacheEntry{ out });
    ::LeaveCriticalSection(&g_cacheCs);

    return out;
}

std::wstring ToWideCp(UINT cp, const char* s, int len)
{
    if (!s) return std::wstring();
    if (len < 0) len = (int)::strlen(s);
    std::wstring out;
    if (!Convert(cp, 0, s, len, &out)) out.clear();
    return out;
}




