# vkshim - Turnip for RDR (com.rockstargames.rdr)

## Build (phone-only)
1. Create a GitHub repo, upload this whole folder (keep the folders), open Actions -> build-vkshim -> Run workflow.
2. Download artifact `vkshim-arm64` -> `lib/arm64-v8a/*.so`. Expect: libvkshim.so + the adrenotools hook libs
   (libhook_impl.so, libmain_hook.so, libfile_redirect_hook.so ...; whatever the build produced is in BUILD_LISTING.txt).
   If the build fails, send me the red step log.

## Patch the game (MT Manager)
1. APK: open lib/arm64-v8a, add ALL the .so files from the artifact.
2. Rename the Turnip driver `vulkan.purple.so` -> `libvulkan_purple.so` and add it to lib/arm64-v8a too
   (Android only extracts files named lib*.so). 
3. librdr.so (get it out of the original APK): hex-replace the single `libvulkan.so` -> `libvkshim.so`
   (6C 69 62 76 6B 73 68 69 6D 2E 73 6F). Put it back, save, sign, install.
4. Do NOT add android:extractNativeLibs="false" - the libs must be real files on disk.
   (Manifest in your APK has no such attribute -> libs are extracted at install = good.)

## v2 note
The driver is now loaded lazily on the first vk call (not in the constructor), because libadrenotools'
internal init (linkernsbypass) had not run yet when v1 called it -> v1 returned NULL with no dlerror.
The log now prints `linkernsbypass_load_status()` and tries 3 call variants (A/B/C).

## After one launch (even a crash) send me, from /sdcard/Android/data/com.rockstargames.rdr/files/vkshim_logs/
- vkshim.log   (everything: device, file checks, dlerror, driver load result, selftest GPU/apiVersion, every vkCreateInstance/Device, null proc addrs)
- vkshim.prev.log (previous run)
- crash_*.txt  (registers, backtrace with module+offset, last 256 log lines, thread list, full maps)
- `adb`-less logcat is not needed; the same lines are also in logcat tag `vkshim`.

Create an empty file named NO_SELFTEST in that folder to skip the throwaway-instance selftest.

## Data folder access (libdatalink.so)
Goal: the game's protected folder `/data/data/com.rockstargames.rdr/...` is editable from
`/sdcard/Android/media/com.rockstargames.rdr/` (MT Manager, any file manager, no root).

How: for every folder in the protected data dir (default: all except cache, code_cache, lib, no_backup, databases) the
files are copied once into `Android/media/<pkg>/<folder>`, the original is renamed `<folder>.datalink_bak`, and a symlink
`/data/data/<pkg>/<folder> -> /storage/emulated/0/Android/media/<pkg>/<folder>` is left. The game then reads/writes
there as if nothing changed. Files already present in the media folder are never overwritten.

Install (MT Manager):
1. Add `libdatalink.so` (from the build artifact) to `lib/arm64-v8a/`.
2. DEX editor -> `classes.dex` -> class `com/rockstargames/rdr/LauncherActivity` (do the same for `NativeGameActivity`
   and `com/an1/unpackdata/MainActivity`; whichever starts first does the work, the rest are no-ops).
   Add / extend the static constructor so it contains these 2 lines at its very start:
```
    const-string v0, "datalink"
    invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
```
   If the class has no `<clinit>`, add:
```
.method static constructor <clinit>()V
    .locals 1
    const-string v0, "datalink"
    invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
    return-void
.end method
```
   If it already has one, put the two lines first and make sure `.locals` is at least 1 (do not clobber v0 later:
   use a register number below `.locals`; if unsure use the shim fallback below).
3. Sign + install, launch once. Read `Android/media/com.rockstargames.rdr/datalink.log`: it lists what was linked and a
   tree (3 levels) of the protected folder, so you can see where the config/savedata really are.

No smali? `libvkshim.so` also runs the same code, but it is loaded late (first Vulkan call), after the game has probably
read its config, so the first launch would still use the old data. Later launches work.

Settings: `Android/media/<pkg>/datalink.cfg` (created on first run): `include X`, `exclude X`, `maxmb N`, `off`.
Folders bigger than `maxmb` (default 1536) are left alone and reported in the log.
Undo: delete the symlinks / clear app data, or put `off` in the cfg.
Caveats: storage under Android/media is FUSE (slower, no exec). Big read-heavy folders (assets) may load slower; use
`exclude`. SQLite databases are excluded by default because locking on FUSE can be unreliable.
