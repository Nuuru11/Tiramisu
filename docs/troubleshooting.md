# Tiramisu — troubleshooting

## Staging / deploy

- **"32-bit exe refused"** — Tiramisu runs PE32+ only. There is no 32-bit path yet.
- **Install fails with 401** — check credentials in `~/.hermes/.env` / dashboard login
  (`admin` / `terminus` by default). `devportal.py` prints the portal's response.
- **Connect error prints `ERROR: …`** — the portal is unreachable or TLS failed.
  Confirm `XBOX_IP`/portal URL and that the device is in Dev Mode.
- **Missing game directory after stage** — `game-stage.py` records `staging.json`;
  re-run with `--set key=value` to write `tiramisu.profile.ini`.

## Runtime

- **Silent audio on Celeste** — expected (FSB5 disabled on console). Try
  `--set fsb_decode=1` on a staged copy and check the console log; do not ship
  with it on until an on-device run confirms no heap corruption.
- **Crash on launch** — check the local log (always written). Named refusal
  messages in the log point at the failing bridge; file an issue with the log
  excerpt (no PII).
- **D3D12 title fails to render** — only the triangle gate is proven; real
  draw paths may be unimplemented. See `docs/known-issues.md`.

## Build / dev

- **`build-test.sh` fails at §34 (settings)** — `settings.cpp` must not reference
  the engine; push FSB-decode state from `main.cpp` instead.
- **Stale `tests/out/libtrt.a`** — the gate starts with `rm -rf trt_objs libtrt.a`;
  a killed run leaves it missing. Re-run the gate.
- **`TRT_JOBS>=3` OOMs** — use `TRT_JOBS=2`.
- **Matrix out of date** — run `tools/title-matrix.py --check`; regenerate with
  `tools/title-matrix.py` after adding a row to `docs/proof/titles.json`.