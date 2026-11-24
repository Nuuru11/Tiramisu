// tiramisu/src/host/cover_art.h
// Real game cover art loader & automatic Steam CDN art fetcher for Xbox UWP.
#ifndef TIRAMISU_HOST_COVER_ART_H
#define TIRAMISU_HOST_COVER_ART_H

#include <cstdint>
#include <string>
#include <vector>

namespace longan {

// Initialize cover art cache under <state_dir>\covers.
void cover_art_init(const std::string& state_dir);

// Request cover art for a game (checks local disk first, otherwise fetches
// Steam header art in background without blocking the UI).
void cover_art_request(const std::string& game_name,
                       const std::string& game_dir, uint32_t steam_appid);

// Save custom uploaded cover bytes (JPEG/PNG) to disk and decode immediately.
bool cover_art_save_bytes(const std::string& game_name, const uint8_t* data,
                          size_t len);

// Get decoded premultiplied BGRA pixels for Direct2D upload (card cover art).
// Returns true if pixels are ready; out_version increments on updates.
bool cover_art_get_bgra(const std::string& game_name,
                        std::vector<uint8_t>* out_bgra, unsigned* out_w,
                        unsigned* out_h, uint64_t* out_version);

// Get decoded premultiplied BGRA pixels for the high-res widescreen Hero banner
// (1920px Steam library_hero / 1080p key art; falls back to card art if absent).
bool cover_art_get_hero_bgra(const std::string& game_name,
                             std::vector<uint8_t>* out_bgra, unsigned* out_w,
                             unsigned* out_h, uint64_t* out_version);

// Return cached cover file path on disk (for serving via webxfer /api/cover).
std::string cover_art_disk_path(const std::string& game_name);

// Clear in-memory queue/cache so cover_art_request re-fetches missing art.
void cover_art_reset_queue();

} // namespace longan

#endif // TIRAMISU_HOST_COVER_ART_H
