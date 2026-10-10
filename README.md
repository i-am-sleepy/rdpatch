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

well game works now I don't care anymore🥲
