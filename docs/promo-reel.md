# 60-second promo / setup reel — shot list and capture recipe

This is the production sheet for the public clip. Every shot is captured from a
real Xbox Series S|X in Dev Mode; do not substitute PC emulation or a Device
Portal slideshow for the gameplay shot. The current `docs/proof/*.mp4` captures
are 2–5 fps Device Portal assemblies and are **not** suitable as the hero clip.

Target: 1920×1080, 60 fps, H.264 High, AAC 48 kHz. Vertical 9:16 cutdown for
Shorts/TikTok is a crop of the same master.

## Storyboard

| Time | Shot | On-screen text | Capture |
|---|---|---|---|
| 0:00–0:05 | Xbox Dev Home, Settings → "Treat UWP apps as games by default" ON | "5 GB RAM. One toggle." | phone camera or capture card |
| 0:05–0:12 | PC browser → `https://<xbox-ip>:11443` → My games & apps → Add → `Tiramisu.msix` + `dev.cer` | "Install from any browser" | screen recording |
| 0:12–0:18 | Launch Tiramisu; chrome boot animation to shelf | "Tiramisu" | HDMI capture, 60 fps |
| 0:18–0:24 | LAN URL + QR on the TV; phone scans it; dashboard opens | "Your phone is the game drive" | phone camera over shoulder |
| 0:24–0:28 | Drag a `.zip` on the dashboard; progress bar; game appears on shelf | "Upload. Pick the .exe. Play." | screen recording |
| 0:28–0:48 | Real gameplay, 60 fps, controller in hand | game title + "native x64 on Xbox" | HDMI capture **only** |
| 0:48–0:54 | Settings → input `pad2kbm` / phone trackpad; cut to KBM edition launching | "No mouse? Controller or phone trackpad. KBM edition for hardware mouse." | HDMI + screen recording |
| 0:54–0:60 | Saves & Support → Backup Saves (.zip) | "Back up before you update." + repo URL | screen recording |

## Capture commands

```sh
# one still / short clip through Device Portal (not the 60 fps hero shot)
python3 tools/console.py screenshot --console <ip> shot.png
python3 tools/console.py proof --game staging/<Title> --console <ip>

# attach the capture card / HDMI recorder for the gameplay segment, then:
ffmpeg -i gameplay.mkv -vf "fps=60,scale=1920:1080" -c:v libx264 -crf 18 \
  -c:a aac -b:a 192k promo-gameplay.mp4
```

## Asset checklist

- [ ] 60 fps gameplay master for at least one verified title (RACCOIN or Celeste)
- [ ] boot animation capture
- [ ] Device Portal install screen recording
- [ ] phone QR scan shot
- [ ] save-backup shot
- [ ] captions burned in and a clean no-caption master
- [ ] thumbnail: `docs/banner-chrome.png` crop + game frame

Do not put a title in the clip that is not in `docs/title-matrix.md` with a real
console artifact. Celeste footage must carry the "silent audio" note until FSB5
decode is fixed on hardware.
