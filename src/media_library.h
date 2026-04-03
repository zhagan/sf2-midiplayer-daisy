#pragma once

#include <cstddef>
#include <cstdint>

namespace major_midi
{

enum class BrowserEntryKind : uint8_t
{
    FxSettings,
    SongSettings,
    Midi,
    SoundFont,
};

struct BrowserEntry
{
    BrowserEntryKind kind  = BrowserEntryKind::Midi;
    size_t           index = 0;
};

class MediaLibrary
{
  public:
    static constexpr size_t kMaxMidiFiles  = 64;
    static constexpr size_t kMaxSoundFonts = 32;
    static constexpr size_t kNameMax       = 32;

    void Scan();

    size_t MidiCount() const { return midi_count_; }
    size_t SoundFontCount() const { return sf2_count_; }
    size_t BrowserEntryCount() const { return 2 + midi_count_ + sf2_count_; }

    const char* MidiName(size_t index) const;
    const char* SoundFontName(size_t index) const;
    void        BuildMidiPath(size_t index, char* out, size_t out_sz) const;
    void        BuildSoundFontPath(size_t index, char* out, size_t out_sz) const;

    BrowserEntry BrowserEntryAt(size_t cursor) const;

  private:
    bool HasExtCaseInsensitive(const char* name, const char* ext) const;
    void ScanDir(const char* path,
                 const char* ext,
                 char        dest[][kNameMax],
                 size_t&     count,
                 size_t      max_count);

    char   midi_files_[kMaxMidiFiles][kNameMax]{};
    char   sf2_files_[kMaxSoundFonts][kNameMax]{};
    size_t midi_count_ = 0;
    size_t sf2_count_  = 0;
};

} // namespace major_midi
