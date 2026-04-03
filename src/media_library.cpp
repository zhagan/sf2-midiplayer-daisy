#include "media_library.h"

#include <cctype>
#include <cstdio>
#include <cstring>

extern "C"
{
#include "ff.h"
}

namespace major_midi
{

bool MediaLibrary::HasExtCaseInsensitive(const char* name, const char* ext) const
{
    if(!name || !ext)
        return false;

    const char* dot = std::strrchr(name, '.');
    if(!dot || dot[1] == '\0')
        return false;

    dot++;
    while(*dot && *ext)
    {
        if(std::tolower(static_cast<unsigned char>(*dot))
           != std::tolower(static_cast<unsigned char>(*ext)))
            return false;
        dot++;
        ext++;
    }

    return *dot == '\0' && *ext == '\0';
}

void MediaLibrary::ScanDir(const char* path,
                           const char* ext,
                           char        dest[][kNameMax],
                           size_t&     count,
                           size_t      max_count)
{
    count = 0;

    DIR     dir;
    FILINFO fno;
    if(f_opendir(&dir, path) != FR_OK)
        return;

    while(f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0')
    {
        if((fno.fattrib & AM_DIR)
           || fno.fname[0] == '_'
           || fno.fname[0] == '.'
           || (fno.fname[0] == '.' && fno.fname[1] == '_')
           || !HasExtCaseInsensitive(fno.fname, ext))
            continue;
        if(count >= max_count)
            break;

        const size_t copy_len
            = std::strlen(fno.fname) < (kNameMax - 1) ? std::strlen(fno.fname)
                                                       : (kNameMax - 1);
        std::memcpy(dest[count], fno.fname, copy_len);
        dest[count][copy_len] = '\0';
        count++;
    }

    f_closedir(&dir);
}

void MediaLibrary::Scan()
{
    ScanDir("0:/midi", "mid", midi_files_, midi_count_, kMaxMidiFiles);
    ScanDir("0:/soundfonts", "sf2", sf2_files_, sf2_count_, kMaxSoundFonts);
}

const char* MediaLibrary::MidiName(size_t index) const
{
    return index < midi_count_ ? midi_files_[index] : "";
}

const char* MediaLibrary::SoundFontName(size_t index) const
{
    return index < sf2_count_ ? sf2_files_[index] : "";
}

void MediaLibrary::BuildMidiPath(size_t index, char* out, size_t out_sz) const
{
    if(out_sz == 0)
        return;
    if(index >= midi_count_)
    {
        out[0] = '\0';
        return;
    }
    std::snprintf(out, out_sz, "0:/midi/%s", midi_files_[index]);
}

void MediaLibrary::BuildSoundFontPath(size_t index, char* out, size_t out_sz) const
{
    if(out_sz == 0)
        return;
    if(index >= sf2_count_)
    {
        out[0] = '\0';
        return;
    }
    std::snprintf(out, out_sz, "0:/soundfonts/%s", sf2_files_[index]);
}

BrowserEntry MediaLibrary::BrowserEntryAt(size_t cursor) const
{
    BrowserEntry entry{};
    if(cursor == 0)
    {
        entry.kind = BrowserEntryKind::FxSettings;
        return entry;
    }

    if(cursor == 1)
    {
        entry.kind = BrowserEntryKind::SongSettings;
        return entry;
    }

    cursor -= 2;
    if(cursor < midi_count_)
    {
        entry.kind  = BrowserEntryKind::Midi;
        entry.index = cursor;
        return entry;
    }

    entry.kind  = BrowserEntryKind::SoundFont;
    entry.index = cursor >= midi_count_ ? (cursor - midi_count_) : 0;
    if(entry.index >= sf2_count_)
        entry.index = sf2_count_ > 0 ? (sf2_count_ - 1) : 0;
    return entry;
}

} // namespace major_midi
