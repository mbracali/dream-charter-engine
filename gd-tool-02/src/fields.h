// gd-tool-02: the layout of project.yaml and the fields of Song home > Details.
#pragma once

#include <string>
#include <utility>
#include <vector>

// A new project.yaml (python/extract_metadata.py has the same SCHEMA). The app adds
// sheets:, forge: and export: as they are used.
inline const std::vector<std::pair<std::string, std::vector<std::string>>> kSchema = {
    {"project", {"name", "created", "source_file", "audio_file", "charter", "notes"}},
    {"song", {"title", "artist", "album", "album_artist", "year", "genre", "track", "disc", "composer", "language", "comment"}},
    {"analysis", {"bpm", "key", "key_confidence", "start_silence_s", "peak_dbfs", "rms_dbfs"}},
    {"audio", {"format", "codec", "duration_s", "sample_rate", "channels", "bitrate_kbps", "file_size_mb", "sha256"}},
    {"stems", {"mode", "model", "status", "error", "separated_at"}},
};

struct FieldSpec {
    const char* section;
    const char* key;
    const char* label;
    bool editable;
};

// Details: the song (left column) and what was measured (right column).
inline const std::vector<FieldSpec> kLeftFields = {
    {"song", "title", "Song title", true},       {"song", "artist", "Artist / band", true},
    {"song", "album", "Album", true},             {"song", "year", "Year", true},
    {"song", "genre", "Genre", true},             {"song", "track", "Track", true},
    {"song", "composer", "Composer", true},       {"song", "language", "Language", true},
    {"project", "charter", "Charter", true},      {"project", "name", "Project name", true},
    {"project", "notes", "Notes", true},
};
inline const std::vector<FieldSpec> kRightFields = {
    {"analysis", "bpm", "BPM", true},
    {"analysis", "key", "Key", true},
    {"analysis", "start_silence_s", "Silence (s)", false},
    {"analysis", "rms_dbfs", "Loudness dB", false},
    {"audio", "format", "Format", false},
    {"audio", "duration_s", "Duration (s)", false},
    {"audio", "sample_rate", "Sample rate", false},
    {"audio", "bitrate_kbps", "Bitrate kbps", false},
    {"audio", "file_size_mb", "Size (MB)", false},
    {"project", "source_file", "Imported", false},
    {"project", "created", "Created", false},
};

inline const FieldSpec& fieldAt(int i) {
    return i < int(kLeftFields.size()) ? kLeftFields[size_t(i)] : kRightFields[size_t(i) - kLeftFields.size()];
}
inline int fieldCount() { return int(kLeftFields.size() + kRightFields.size()); }
