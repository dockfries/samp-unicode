// Checks that the values written into a fresh samp-unicode.ini match the ones the code falls back
// to when a key is missing.
//
// This is not a style check. A first run used to write the template and then read nothing, so it
// fell back to the IniGet defaults - and those disagreed with the template about antialiasing and
// about whether the client's own shadow copies are dropped. A fresh install therefore drew a black
// shadow behind the chat, and the second launch looked right: impossible to diagnose from a
// screenshot, which is why it went unnoticed.
//
// Both sides are read out of src/config.cpp: the template is the wide string literal kTemplate, the
// defaults are the IniGetStr/Int/Float calls. No game, no ini file, no Python.

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what, const std::string& detail = "")
{
    if (ok) { std::printf("    ok    %s\n", what.c_str()); return; }
    ++g_failures;
    std::printf("    FAIL  %s %s\n", what.c_str(), detail.c_str());
}

std::string ReadFile(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string Lower(std::string s)
{
    for (std::string::iterator i = s.begin(); i != s.end(); ++i) *i = (char)::tolower((unsigned char)*i);
    return s;
}

// The template as a real ini text: every L"...\r\n" literal in kTemplate, with the escape
// sequences turned back into characters.
std::string TemplateText(const std::string& src)
{
    const size_t decl = src.find("static const wchar_t* kTemplate =");
    if (decl == std::string::npos) return std::string();
    size_t end = src.find("verbose = off", decl);
    if (end == std::string::npos) return std::string();
    end = src.find(';', end);
    if (end == std::string::npos) return std::string();

    std::string out;
    size_t i = decl;
    while (true) {
        i = src.find("L\"", i);
        if (i == std::string::npos || i > end) break;
        i += 2;
        std::string piece;
        while (i < src.size() && src[i] != '"') {
            if (src[i] == '\\' && i + 1 < src.size()) {
                const char esc = src[i + 1];
                if (esc == 'r' || esc == 'n') piece += '\n';
                else if (esc == 't') piece += '\t';
                else piece += esc;
                i += 2;
                continue;
            }
            piece += src[i++];
        }
        out += piece;
        if (i < src.size()) ++i;
    }
    return out;
}

std::map<std::string, std::string> ParseTemplate(const std::string& text)
{
    std::map<std::string, std::string> out;
    std::string section;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') {
            section = Trim(line.substr(1, line.find_last_of(']') - 1));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, eq));
        out[(section.empty() ? "" : section + ".") + key] = Trim(line.substr(eq + 1));
    }
    return out;
}

// key -> the default given to IniGetStr/IniGetInt/IniGetFloat.
std::map<std::string, std::string> ParseDefaults(const std::string& src)
{
    std::map<std::string, std::string> out;
    size_t i = 0;
    while ((i = src.find("IniGet", i)) != std::string::npos) {
        const size_t open = src.find('(', i);
        if (open == std::string::npos) break;
        // Only calls whose first argument is a literal key. A call with a variable there would
        // otherwise pick up the *default* string as the key and desynchronise the scan.
        size_t keyStart = open + 1;
        while (keyStart < src.size() && (src[keyStart] == ' ' || src[keyStart] == '\t')) ++keyStart;
        if (keyStart + 1 >= src.size() || src[keyStart] != 'L' || src[keyStart + 1] != '"') {
            i = open + 1;
            continue;
        }
        const size_t keyEnd = src.find('"', keyStart + 2);
        if (keyEnd == std::string::npos) break;
        const size_t comma = src.find(',', keyEnd);
        if (comma == std::string::npos) break;
        const std::string key = src.substr(keyStart + 2, keyEnd - keyStart - 2);

        size_t v = comma + 1;
        while (v < src.size() && (src[v] == ' ' || src[v] == '\t')) ++v;
        std::string value;
        if (v + 1 < src.size() && src[v] == 'L' && src[v + 1] == '"') {
            const size_t e = src.find('"', v + 2);
            value = src.substr(v + 2, e - v - 2);
            i = e;
        } else {
            size_t e = v;
            while (e < src.size() && src[e] != ',' && src[e] != ')') ++e;
            value = Trim(src.substr(v, e - v));
            i = e;
        }
        out[key] = Trim(value);
    }
    return out;
}

// Template text against default text: equal as numbers when both are numbers, else equal
// ignoring case (a trailing 'f' is C++ syntax, not a value).
bool Same(const std::string& a, const std::string& b)
{
    if (Lower(Trim(a)) == Lower(Trim(b))) return true;
    const char* pa = a.c_str();
    const char* pb = b.c_str();
    char* ea = nullptr;
    char* eb = nullptr;
    const double da = std::strtod(pa, &ea);
    const double db = std::strtod(pb, &eb);
    if (ea == pa || eb == pb) return false;
    return (da - db < 1e-6) && (db - da < 1e-6);
}

// The map is const here; operator[] would default-construct a missing key.
std::string Value(const std::map<std::string, std::string>& m, const char* key)
{
    const std::map<std::string, std::string>::const_iterator it = m.find(key);
    return it == m.end() ? std::string() : it->second;
}

} // namespace

int main()
{
    const std::string src = ReadFile("../src/config.cpp");
    if (src.empty()) {
        std::printf("FAILED: cannot read ../src/config.cpp\n");
        return 1;
    }

    const std::string text = TemplateText(src);
    const std::map<std::string, std::string> tmpl = ParseTemplate(text);
    const std::map<std::string, std::string> defaults = ParseDefaults(src);

    std::printf("the values a fresh ini gets\n");
    Check(!tmpl.empty(), "the template was found");
    Check(defaults.size() > 20, "the IniGet defaults were found");

    std::printf("\nevery key the template sets has the same default\n");
    int differ = 0;
    for (std::map<std::string, std::string>::const_iterator it = tmpl.begin(); it != tmpl.end(); ++it) {
        const std::map<std::string, std::string>::const_iterator d = defaults.find(it->first);
        if (d == defaults.end()) continue;                       // reported below
        // font0..font3 are read through a loop over the four slots.
        if (!Same(d->second, it->second)) {
            ++differ;
            std::printf("    -> %s: default '%s' != template '%s'\n",
                        it->first.c_str(), d->second.c_str(), it->second.c_str());
        }
    }
    Check(differ == 0, "all keys agree", differ ? "(see above)" : "");

    std::printf("\nevery key the template sets is read by the code\n");
    std::vector<std::string> unread;
    for (std::map<std::string, std::string>::const_iterator it = tmpl.begin(); it != tmpl.end(); ++it) {
        if (defaults.count(it->first)) continue;
        // font0..font3 are read through a loop over the four slots, so they have no literal default.
        if (it->first.size() == 10 && it->first.compare(0, 5, "font.") == 0 &&
            it->first.compare(5, 4, "font") == 0 && std::isdigit((unsigned char)it->first[9]))
            continue;
        unread.push_back(it->first);
    }
    std::string list;
    for (size_t i = 0; i < unread.size(); i++) list += (i ? ", " : "") + unread[i];
    Check(unread.empty(), "no key is written but never read", list.empty() ? "" : "(" + list + ")");

    std::printf("\nthe settings that made a first run draw a black shadow\n");
    Check(Lower(Value(defaults, "paths.client_text_drop_shadows")) == "on", "drop_shadows defaults to on");
    Check(Lower(Value(tmpl, "paths.client_text_drop_shadows")) == "on", "and the template says on");
    Check(Trim(Value(defaults, "paths.client_text_shadow_alpha")) == "255", "shadow_alpha defaults to 255");
    Check(Lower(Value(defaults, "font.antialias")) == "on", "antialias defaults to on");

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL CHECKS PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
