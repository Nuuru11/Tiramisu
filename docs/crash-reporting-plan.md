# Tiramisu — crash reporting plan (not implemented)

Status: **planned, not built.** Opt-in, disable-anytime, privacy-preserving,
reports to a repo issue tracker without user accounts or maintainer infra.

## Goal

Auto-report crashes/errors so the maintainer sees them, with zero end-user
accounts and ~2 minutes of one-time maintainer setup. No tokens in the binary.

## Maintainer setup (one-time, ~2 min) — pick one

- **Option A (recommended):** Sentry free tier → create project → paste DSN into
  build constant. Gives grouping, dedup, stack traces, alerts.
- **Option B (zero SDK):** Formspree/Google Form → create form → paste POST URL.
  Raw entries/emails, no grouping.

Endpoint URL/DSN is a compile-time constant (`TIRAMISU_CRASH_ENDPOINT`); empty
by default → feature inert until set.

## Architecture

1. **Setting** `crash_report = false` in `host_settings` (ini key `crash_report`),
   mirroring `fsb_decode`: settings.ini + per-game `tiramisu.profile.ini` +
   `push_crash_report()` from `main.cpp`. Default off.
2. **Capture** in `main.cpp`: `SetUnhandledExceptionFilter` + `__try/__except`
   around the guest run loop. Always write a local log/minidump first.
3. **If opted in**: build allowlisted payload, POST asynchronously (detached
   thread, short timeout). Never block the crash path.
4. **Endpoint**: maintainer-hosted receiver (Sentry SDK or plain HTTP POST) that
   files/dedupes into repo issues using a server-side token.

## Privacy allowlist (send only these)

- App version + build date, OS build, arch
- Crash signature: exception code, faulting module+offset, top ~8 frames as
  module+offset
- Active bridge subsystem (from `trt_trace`/log lines)
- Behavioral settings (`fsb_decode`, etc.)
- Hashed title fingerprint (not name/path) — separate sub-toggle

## Never send

Usernames, paths, save data, game content, full minidumps, IP, file listings,
screenshots.

## UX / trust

- First-run prompt with "what we send" preview.
- Settings row: last report timestamp + replay of exact payload sent.
- One-click "disable + forget my client id" (random UUID on opt-in; server
  forgets on request).
- Local log always written so opt-out users can still file issues manually.

## Build order (when started)

1. Setting toggle + `push_crash_report` plumbing.
2. Crash filter writing local log + building payload (no network).
3. Dashboard dry-run preview of the payload (audit before any network code).
4. Network POST behind the toggle.

## Helper

`tools/crash-report-preview.py` — renders a sample payload for review.