// gd-tool-02: a multi-line UTF-8 text being edited (the lyrics editor, the Paste editor):
// the text, the caret (a byte offset) and the first visible line.
#pragma once

#include <algorithm>
#include <string>
#include <utility>

#include "text_util.h"

struct TextEdit {
    std::string text;
    size_t caret = 0;
    int scroll = 0;
    bool dirty = false;

    void set(std::string s) {
        text.clear();
        for (char c : s)
            if (c != '\r') text += c;
        caret = 0;
        scroll = 0;
        dirty = false;
    }
    void insert(const std::string& s) {
        std::string clean;
        for (char c : s)
            if (c != '\r') clean += c;
        text.insert(caret, clean);
        caret += clean.size();
        dirty = true;
    }
    void backspace() {
        if (caret == 0) return;
        const size_t from = text::prevStart(text, caret);
        text.erase(from, caret - from);
        caret = from;
        dirty = true;
    }
    void del() {
        if (caret >= text.size()) return;
        size_t to = caret;
        text::next(text, to);
        text.erase(caret, to - caret);
        dirty = true;
    }
    void left() {
        if (caret > 0) caret = text::prevStart(text, caret);
    }
    void right() {
        if (caret < text.size()) text::next(text, caret);
    }
    int lineTotal() const { return int(std::count(text.begin(), text.end(), '\n')) + 1; }
    size_t lineStart(int line) const {
        size_t i = 0;
        for (int l = 0; l < line; ++l) {
            const size_t nl = text.find('\n', i);
            if (nl == std::string::npos) return text.size();
            i = nl + 1;
        }
        return i;
    }
    // The caret as (line, column in characters).
    std::pair<int, int> pos() const {
        int line = 0;
        size_t start = 0;
        for (size_t i = 0; i < caret && i < text.size(); ++i)
            if (text[i] == '\n') {
                ++line;
                start = i + 1;
            }
        return {line, int(text::count(text.substr(start, caret - start)))};
    }
    void moveTo(int line, int col) {
        line = std::clamp(line, 0, lineTotal() - 1);
        const size_t start = lineStart(line);
        const size_t end = text.find('\n', start);
        const std::string l = text.substr(start, (end == std::string::npos ? text.size() : end) - start);
        caret = start + text::byteOf(l, size_t(std::max(0, col)));
    }
    // Lines that are not blank.
    int filledLines() const {
        int n = 0;
        size_t start = 0;
        while (start <= text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            if (text.find_first_not_of(" \t", start) < end) ++n;
            start = end + 1;
        }
        return n;
    }
};
