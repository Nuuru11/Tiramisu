# Sekiro .166 fix: swallow the thread-naming exception (0x406d1388)

## What happened
Sekiro on .165 (master) passed the locale gate for the first time: imports
cleared, Steam RestartAppIfNecessary returned 0, GetCurrentGameLanguage
returned english, zero 0xe06d7363 / bad locale name lines. The guest then
died in ~2s on guest fault 0xc0000005 at sekiro.exe+0x115c66b (read
address NULL-24), immediately after RaiseException(code=0x406d1388), the
MSVC thread-naming exception.

## Root cause
The 0x406d1388 swallow exists only in the SysV path trt_raise_exception_sysv
(src/trt/bridge_kernel32.cpp:5458). The Win64 UWP path trt_RaiseException
(:5734) has no swallow, so the benign thread-naming exception goes through
full VEH dispatch and the guest faults right after. Evidence: the log shows
the generic RaiseException(...) line, not the swallowed line.

## Fix (6 lines)
Add at the TOP of trt_RaiseException, before the log block:

    if (code == 0x406d1388u) {
      boot_log("kernel32: RaiseException(0x406d1388) thread-name exception "
               "swallowed (no debugger attached)");
      return;
    }

## After applying
Build + deploy .166 from master, then relaunch Sekiro. If the fault clears,
the next gate is D3D/window/file activity or menu. FPS patch 14 presence in
master is unknown; check for trt: frame avg lines if the game renders.

## Full log
/tmp/tiramisu165-post-sekiro.log on mineplex (106,779 B). Lane note:
~/tiramisu-gow-skye/docs/progress/woowop-lane.md
