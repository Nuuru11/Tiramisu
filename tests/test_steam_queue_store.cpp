#include "../src/host/steam_queue_store.h"
#include "../src/host/game_video.h"
#include <cassert>
#include <cstdlib>

int main() {
  char folder[] = "/tmp/tiramisu-queue-profile-XXXXXX";
  assert(mkdtemp(folder));
  const auto root = std::filesystem::path(folder);
  const auto path = root / "steam-queue-123.txt";
  using namespace longan;
  std::vector<steam_queue_entry> jobs{
    {4, true, "Family \"A\"", "holding", "", 99},
    {3, false, "My\ngame", "running", "", 100},
    {8, true, "Shared", "confirm", "Required 3 GiB", 101}
  };
  assert(steam_queue_store::save(path, jobs));
  std::vector<steam_queue_entry> loaded;
  assert(steam_queue_store::load(path, loaded) && loaded.size() == 3);
  assert(loaded[0].app_id == 4 && loaded[0].family && loaded[0].state == "paused" && loaded[0].ticket == 0);
  assert(loaded[1].state == "queued" && loaded[1].title == jobs[1].title && !loaded[1].family);
  assert(loaded[2].state == "paused" && !loaded[2].detail.empty());
  assert(steam_queue_store::save(path, {jobs[1]}));
  assert(steam_queue_store::load(path, loaded) && loaded.size() == 1 && loaded[0].app_id == 3);
  std::ofstream(path) << "TQ1 3\n7 0 queued";
  assert(!steam_queue_store::load(path, loaded) && loaded.size() == 1);
  std::ofstream(root / "tiramisu_video.txt") << "default on 720p";
  game_video::load(folder);
  assert(game_video::hd720 && game_video::cap == 30);
  std::ofstream(root / "tiramisu_video.txt") << "60 off";
  game_video::load(folder);
  assert(!game_video::hd720 && game_video::cap == 60);
  std::filesystem::remove_all(root);
}
