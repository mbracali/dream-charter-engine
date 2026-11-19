// gd-tool-02: UTF-8 helpers. The data is UTF-8 everywhere; SDL's debug font
// draws ASCII only, so ascii() shows each character as one ASCII character
// (accents dropped: "Canção" -> "Cancao"), which keeps caret maths simple.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace text {

// The code point starting at s[i]; advances i past it.
inline uint32_t next(const std::string& s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 0;
    uint32_t cp = n == 0 ? c : n == 1 ? (c & 0x1F) : n == 2 ? (c & 0x0F) : (c & 0x07);
    ++i;
    for (int k = 0; k < n && i < s.size(); ++k, ++i) cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
    return cp;
}

inline size_t prevStart(const std::string& s, size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    return i;
}

inline size_t count(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++n) next(s, i);
    return n;
}

// Byte offset of the n-th code point (clamped to the end).
inline size_t byteOf(const std::string& s, size_t n) {
    size_t i = 0;
    for (size_t k = 0; k < n && i < s.size(); ++k) next(s, i);
    return i;
}

inline char asciiOf(uint32_t cp) {
    if (cp >= 0x20 && cp < 0x7F) return char(cp);
    if (cp == '\t') return ' ';
    static const char* latin1 = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (cp >= 0xC0 && cp <= 0xFF) return latin1[cp - 0xC0];
    switch (cp) {
        case 0x2018: case 0x2019: case 0x00B4: return '\'';
        case 0x201C: case 0x201D: return '"';
        case 0x2013: case 0x2014: return '-';
        case 0x2026: return '.';
        case 0x00A0: return ' ';
        case 0x0152: return 'O';
        case 0x0153: return 'o';
        case 0x00AA: return 'a';  // 2ª
        case 0x00BA: case 0x00B0: return 'o';  // 1º, degrees
        case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: return '\0';  // invisible: dropped
        default: return '?';
    }
}

inline std::string ascii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();)
        if (const char c = asciiOf(next(s, i))) out += c;
    return out;
}

}  // namespace text
