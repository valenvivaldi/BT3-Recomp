#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// [customsongs] Songs from a folder, appended after the game's own entries in BT3's BGM selector.
//
// The catalog is scanned once at boot: PS2X_CUSTOM_SONGS_PATH, else <exeDir>/songs (on macOS the
// bundle links songs/ to Application Support like data/ and savedata/). PS2X_CUSTOM_SONGS=0 turns
// the feature off. Any format FFmpeg decodes is accepted.
struct CustomSong
{
    std::filesystem::path path;
    std::string title;   // the file name without its extension
};

namespace customsongs
{
    void loadCatalog(const std::filesystem::path &exeDir);
    const std::vector<CustomSong> &songs();
    bool enabled();   // a catalog with at least one song

    // Playback on the host, beside the guest's BGM pair: while a song plays the pair keeps being fed (the IOP's
    // credit accounting is unchanged) but is mixed at zero. play()/stop() may be called from any thread; the
    // decoder runs from service(), on the render thread with the rest of the audio.
    void play(size_t index);
    void stop();
    bool active();       // a song is selected (the guest BGM pair is muted)
    int activeIndex();   // -1 when none
    void service();

    // [bgmmenu] BT3's BGM Select (Map Select -> the note icon). The menu is data-driven: base = *0x3b38d4, the list
    // widget at base+0x554 (cursor at +0x4c), the entry count at base+0x3810 (21) and the table of BGM ids at
    // base+0x3808 ({0..19, 24}; 24 = Random). Each frame, game thread: the table is copied to a block above the guest
    // heap with one id-24 entry per song appended and the count raised, so the game's own list scrolls onto them.
    void bgmMenuTick(uint8_t *rdram, uint64_t frame);
    // The song under the BGM Select cursor, -1 when the menu is closed or on a built-in entry (label rewrite).
    int menuSong();
    // [bgmmenu] GIF packets on their way to the GS: the label upload holding the Random plate gets the title of the
    // song under the cursor. Returns data, or scratch holding the rewritten packet.
    const uint8_t *filterGifPacket(const uint8_t *data, uint32_t size, std::vector<uint8_t> &scratch);
    // [bgmmenu] a fight update ran (bt3LogicRate): the committed song plays while fights keep updating.
    void noteFightUpdate(uint64_t frame);
}
