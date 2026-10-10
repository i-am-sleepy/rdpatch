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

## Data folder access (libdatalink.so) - v2
Goal: the game's protected folder `/data/data/com.rockstargames.rdr/...` is editable from
`/sdcard/Android/media/com.rockstargames.rdr/` (MT Manager, any file manager, no root).

How: a folder that is small (< `maxmb`) and has no excluded content is copied once to `Android/media/<pkg>/<folder>` and
replaced by ONE symlink (old copy kept as `<folder>.datalink_bak`). A folder that is too big or contains an excluded path
(for this game: `files`, because `files/assetpacks` is ~3.7 GB) stays a REAL folder and its contents are linked one by one:
`files/options.xml -> Android/media/<pkg>/files/options.xml`, saves, graphics.xml, small subfolders, ... Big/excluded parts
(assetpacks) stay where they are. First-migration copies of single files are kept in `/data/data/<pkg>/datalink_bak/`.

Rules on every launch:
- new file created by the game in a linked-by-parts folder -> moved to media + linked (so it shows up after the next launch)
- game replaced a link with a fresh file (save written via temp+rename) -> the NEWER of the two (by modified time) wins
- a file/folder that exists only in the media folder -> linked into the data folder, so you can drop files in
- files already in media are never overwritten by an older data copy

Install (MT Manager): add `libdatalink.so` to `lib/arm64-v8a/`, then in the DEX editor add to the static constructor of
`com/rockstargames/rdr/LauncherActivity`, `NativeGameActivity` and `com/an1/unpackdata/MainActivity`:
```
    const-string v0, "datalink"
    invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
```
(no `<clinit>`? add `.method static constructor <clinit>()V` / `.locals 1` / the two lines / `return-void` / `.end method`).
Whichever class starts first does the work; the rest are no-ops. `libvkshim.so` runs the same code as a late fallback.

Settings `Android/media/<pkg>/datalink.cfg`: `include <path>` (alias `includefile`, `file`; paths relative to /data/data/<pkg>),
`exclude <pattern>` (* wildcards), `maxmb N`, `off`. Log: `datalink.log` in the same folder (appended, with a 3-level tree).
Built-in excludes: vkshim scratch folders, androidx profileinstaller marker files.
Undo: delete the symlinks / clear app data, or put `off` in the cfg.
Caveats: Android/media is FUSE (slower, no exec). SQLite `databases` are excluded by default (locking on FUSE).
