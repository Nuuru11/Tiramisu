# Console-type autodetect (Series X/S, Xbox One X/S)

`tools/console_type.py` asks Device Portal (`/api/os/info`) what console it is
on and emits a small “tiramisu tweak” the user can apply before a session.

```sh
tools/console_type.py detect --console <ip> [--user U --password P]
tools/console_type.py detect --console <ip> --apply   # also writes LocalState/console_tweak_<family>.json
```

## Detection

Primary key: `Platform.Model` from `/api/os/info`.
Fallbacks: `ConsoleType`, `ProductId`, `ConsoleModel`, `OSModel`, `MachineName`.
Aliases live in `tools/console_type.py::MODEL_ALIASES`:

| Console | Family |
|---|---|
| Xbox Series X | `series_x` |
| Xbox Series S | `series_s` |
| Xbox One X | `one_x` |
| Xbox One S | `one_s` |

Unknown models are reported honestly (`family: null`), not guessed.

## Tweaks (defaults to validate on hardware)

```json
{
  "series_x": { "memory_budget_mb": 5120, "os_build": 22621 },
  "series_s": { "memory_budget_mb": 5120, "os_build": 22621 },
  "one_x":    { "memory_budget_mb": 4096, "os_build": 22621 },
  "one_s":    { "memory_budget_mb": 3072, "os_build": 19045 }
}
```

These are starting points. Don’t use them as claims without a console proof:

- Series S is the measured baseline (`docs/title-matrix.md`).
- One X is a new recommendation, less GPU/memory headroom.
- One S is the most conservative; heavy titles will still fail.

## Status

- Detection/apply engine: offline, tested (`tests/py_tools/test_console_type.py`).
- The actual settings are consumed by `src/host/settings.cpp` + `/api/settings`.
