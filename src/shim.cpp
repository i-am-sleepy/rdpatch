// libvkshim.so - stands in for libvulkan.so inside librdr.so (patched string).
// 1) Loads a custom Turnip driver through libadrenotools and gets the Android Vulkan loader
//    that sits on top of it.
// 2) Exports every vk* name (trampolines in tramp.S) and forwards it to that loader.
// 3) Writes a very detailed log + crash reports (see README).
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <sys/utsname.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <adrenotools/driver.h>

#define HID __attribute__((visibility("hidden")))
#define EXP __attribute__((visibility("default")))

#include "datalink.h"
#include "names.inc"

extern "C" {
HID void* vkshim_tbl[VKSHIM_COUNT];
HID void vkshim_ret0();                // asm: returns 0
bool linkernsbypass_load_status();     // from libadrenotools' linkernsbypass (static)
}

// ------------------------------------------------------------------ logger
namespace {
std::string g_pkg, g_logdir, g_intdir, g_nlib;
int g_fd = -1;
std::mutex g_logm;
constexpr int kRing = 256;
char g_ring[kRing][320];
int g_ringPos = 0;
bool g_custom = false;
void* g_vk = nullptr;  // handle of the loader (custom-driver backed or system fallback)

double nowSec() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logf(const char* fmt, ...) {
  char body[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof body, fmt, ap);
  va_end(ap);
  char line[1200];
  int n = snprintf(line, sizeof line, "[%10.3f][p%d t%d] %s\n", nowSec(), getpid(), (int)gettid(), body);
  if (n <= 0) return;
  if (n > (int)sizeof line - 1) n = sizeof line - 1;
  std::lock_guard<std::mutex> lk(g_logm);
  strncpy(g_ring[g_ringPos % kRing], line, 319);
  g_ring[g_ringPos % kRing][319] = 0;
  g_ringPos++;
  if (g_fd >= 0) {
    (void)!write(g_fd, line, n);
    fsync(g_fd);  // survive hard kills
  }
  __android_log_write(ANDROID_LOG_INFO, "vkshim", body);
}

int g_serialize = 0;   // SERIALIZE_SUBMITS: 1 = wait for the queue after every submit, 2 = wait for the whole device
bool g_verbose = false;  // VKSHIM_VERBOSE=1 in tu_env.txt: every traced call also goes to the log file

// like logf, but normally only goes into the in-memory ring (so it shows up in crash_*.txt) - cheap enough for per-frame calls
void tracef(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void tracef(const char* fmt, ...) {
  char body[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof body, fmt, ap);
  va_end(ap);
  char line[640];
  int n = snprintf(line, sizeof line, "[%10.3f][p%d t%d] %s\n", nowSec(), getpid(), (int)gettid(), body);
  if (n <= 0) return;
  if (n > (int)sizeof line - 1) n = sizeof line - 1;
  std::lock_guard<std::mutex> lk(g_logm);
  strncpy(g_ring[g_ringPos % kRing], line, 319);
  g_ring[g_ringPos % kRing][319] = 0;
  g_ringPos++;
  if (g_verbose && g_fd >= 0) (void)!write(g_fd, line, n);
}

// writes the newest n ring lines to the log file (used when the GPU is first reported lost)
void dumpRecent(int n) {
  std::lock_guard<std::mutex> lk(g_logm);
  if (g_fd < 0) return;
  int start = g_ringPos > n ? g_ringPos - n : 0;
  dprintf(g_fd, "---- last %d events before this point ----\n", g_ringPos - start);
  for (int i = start; i < g_ringPos; i++) dprintf(g_fd, "%s", g_ring[i % kRing]);
  dprintf(g_fd, "---- end of events ----\n");
  fsync(g_fd);
}

void mkdirs(const std::string& p) {
  std::string cur;
  for (size_t i = 0; i < p.size(); i++) {
    cur += p[i];
    if ((p[i] == '/' || i + 1 == p.size()) && cur.size() > 1) mkdir(cur.c_str(), 0771);
  }
}

bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }
long fsize(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 ? (long)st.st_size : -1; }

std::string readSmall(const char* path, size_t max = 8192) {
  std::string out;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return std::string("<open failed: ") + strerror(errno) + ">";
  char buf[4096];
  ssize_t r;
  while (out.size() < max && (r = read(fd, buf, sizeof buf)) > 0) out.append(buf, r);
  close(fd);
  return out;
}

void dumpFile(int fd, const char* title, const char* path) {
  std::string s = readSmall(path, 1 << 20);
  dprintf(fd, "\n==== %s (%s) ====\n%s\n", title, path, s.c_str());
}

std::string prop(const char* k) {
  char v[PROP_VALUE_MAX] = {0};
  __system_property_get(k, v);
  return v;
}

void listDir(const std::string& dir) {
  logf("  dir %s", dir.c_str());
  FILE* p = popen(("ls -la '" + dir + "' 2>&1").c_str(), "r");
  if (!p) { logf("    (popen failed)"); return; }
  char b[512];
  while (fgets(b, sizeof b, p)) { b[strcspn(b, "\n")] = 0; logf("    %s", b); }
  pclose(p);
}

void logMapsMatching(const char* pat1, const char* pat2 = nullptr) {
  FILE* f = fopen("/proc/self/maps", "r");
  if (!f) return;
  char b[1024];
  int n = 0;
  while (fgets(b, sizeof b, f)) {
    if (strstr(b, pat1) || (pat2 && strstr(b, pat2))) { b[strcspn(b, "\n")] = 0; logf("  maps: %s", b); n++; }
  }
  fclose(f);
  if (!n) logf("  maps: no lines matching '%s'", pat1);
}

void initPaths() {
  char cl[256] = {0};
  int fd = open("/proc/self/cmdline", O_RDONLY);
  if (fd >= 0) { (void)!read(fd, cl, sizeof cl - 1); close(fd); }
  g_pkg = cl;
  if (g_pkg.empty()) g_pkg = "com.rockstargames.rdr";
  // Android/media/<package> first (easy to reach with any file manager), Android/data as a fallback
  const char* ext[] = {"/storage/emulated/0/Android/media/", "/sdcard/Android/media/",
                       "/storage/emulated/0/Android/data/", "/sdcard/Android/data/", nullptr};
  for (int i = 0; ext[i]; i++) {
    bool media = strstr(ext[i], "/media/") != nullptr;
    std::string d = std::string(ext[i]) + g_pkg + (media ? "/vkshim_logs" : "/files/vkshim_logs");
    mkdirs(d);
    if (exists(d) && access(d.c_str(), W_OK) == 0) { g_logdir = d; break; }
  }
  const char* in[] = {"/data/user/0/", "/data/data/", nullptr};
  for (int i = 0; in[i]; i++) {
    std::string d = std::string(in[i]) + g_pkg;
    if (exists(d)) { g_intdir = d; break; }
  }
  if (g_logdir.empty() && !g_intdir.empty()) {
    g_logdir = g_intdir + "/files/vkshim_logs";
    mkdirs(g_logdir);
  }
  Dl_info di;
  if (dladdr((void*)&initPaths, &di) && di.dli_fname) {
    std::string p = di.dli_fname;
    size_t s = p.rfind('/');
    g_nlib = s == std::string::npos ? "." : p.substr(0, s);
  }
  if (!g_logdir.empty()) {
    rename((g_logdir + "/vkshim.log").c_str(), (g_logdir + "/vkshim.prev.log").c_str());
    g_fd = open((g_logdir + "/vkshim.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
  }
}

// ------------------------------------------------------------------ crash handler
struct Bt { uintptr_t pc[64]; int n; };
_Unwind_Reason_Code unwindCb(_Unwind_Context* c, void* arg) {
  Bt* b = (Bt*)arg;
  uintptr_t pc = _Unwind_GetIP(c);
  if (pc && b->n < 64) b->pc[b->n++] = pc;
  return b->n >= 64 ? _URC_END_OF_STACK : _URC_NO_REASON;
}
void printAddr(int fd, uintptr_t a) {
  Dl_info di;
  if (dladdr((void*)a, &di) && di.dli_fname)
    dprintf(fd, "  %016lx  %s+0x%lx  (sym %s+0x%lx)\n", (unsigned long)a, di.dli_fname,
            (unsigned long)(a - (uintptr_t)di.dli_fbase), di.dli_sname ? di.dli_sname : "?",
            di.dli_saddr ? (unsigned long)(a - (uintptr_t)di.dli_saddr) : 0UL);
  else
    dprintf(fd, "  %016lx  <unknown>\n", (unsigned long)a);
}

struct sigaction g_old[32];
const int kSigs[] = {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE, SIGTRAP, SIGSYS};
volatile sig_atomic_t g_inCrash = 0;
int g_crashSeq = 0;

void crashHandler(int sig, siginfo_t* si, void* uc_) {
  if (g_inCrash) { signal(sig, SIG_DFL); return; }
  g_inCrash = 1;
  ucontext_t* uc = (ucontext_t*)uc_;
  char path[512];
  snprintf(path, sizeof path, "%s/crash_%ld_%d.txt", g_logdir.c_str(), (long)time(nullptr), g_crashSeq++);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0660);
  if (fd < 0) fd = g_fd;
  if (fd >= 0) {
    char tn[32] = {0};
    prctl(PR_GET_NAME, tn);
    dprintf(fd, "==== vkshim crash report ====\npackage=%s pid=%d tid=%d thread=%s uptime=%.3f\n", g_pkg.c_str(),
            getpid(), (int)gettid(), tn, nowSec());
    dprintf(fd, "signal=%d (%s) code=%d errno=%d fault_addr=%p\n", sig, strsignal(sig), si->si_code, si->si_errno,
            si->si_addr);
    dprintf(fd, "custom_driver_loaded=%d vk_handle=%p\n", (int)g_custom, g_vk);
    mcontext_t& m = uc->uc_mcontext;
    dprintf(fd, "\n---- registers ----\n");
    for (int i = 0; i < 31; i++) dprintf(fd, "x%-2d=%016llx%s", i, (unsigned long long)m.regs[i], (i % 3 == 2) ? "\n" : "  ");
    dprintf(fd, "\nsp =%016llx pc =%016llx pstate=%016llx\n", (unsigned long long)m.sp, (unsigned long long)m.pc,
            (unsigned long long)m.pstate);
    dprintf(fd, "\n---- fault pc / lr ----\n");
    printAddr(fd, m.pc);
    printAddr(fd, m.regs[30]);
    dprintf(fd, "\n---- unwind backtrace (this signal frame) ----\n");
    Bt bt = {};
    _Unwind_Backtrace(unwindCb, &bt);
    for (int i = 0; i < bt.n; i++) printAddr(fd, bt.pc[i]);
    dprintf(fd, "\n---- frame-pointer walk from faulting context ----\n");
    uintptr_t fp = m.regs[29];
    for (int i = 0; i < 40 && fp && !(fp & 7); i++) {
      uintptr_t* f = (uintptr_t*)fp;
      // validate readable via write() to /dev/null trick is overkill; guard with simple range check
      if (fp < 0x1000) break;
      uintptr_t next = f[0], ret = f[1];
      printAddr(fd, ret);
      if (next <= fp) break;
      fp = next;
    }
    dprintf(fd, "\n---- last %d log lines ----\n", kRing);
    int start = g_ringPos > kRing ? g_ringPos - kRing : 0;
    for (int i = start; i < g_ringPos; i++) dprintf(fd, "%s", g_ring[i % kRing]);
    dumpFile(fd, "status", "/proc/self/status");
    dumpFile(fd, "meminfo", "/proc/meminfo");
    dprintf(fd, "\n==== threads ====\n");
    FILE* p = popen("ls /proc/self/task 2>/dev/null", "r");
    if (p) {
      char b[64];
      while (fgets(b, sizeof b, p)) {
        b[strcspn(b, "\n")] = 0;
        char cp[96];
        snprintf(cp, sizeof cp, "/proc/self/task/%s/comm", b);
        std::string c = readSmall(cp, 64);
        c.erase(c.find_last_not_of("\n") + 1);
        dprintf(fd, "  tid %s : %s\n", b, c.c_str());
      }
      pclose(p);
    }
    std::string maps = readSmall("/proc/self/maps", 4 << 20);
    dprintf(fd, "\n==== maps ====\n%s\n", maps.c_str());
    fsync(fd);
    if (fd != g_fd) close(fd);
  }
  __android_log_print(ANDROID_LOG_FATAL, "vkshim", "CRASH sig=%d addr=%p pc=%llx -> %s", sig, si->si_addr,
                      (unsigned long long)uc->uc_mcontext.pc, path);
  // hand over to the previous handler (debuggerd) so the normal tombstone still happens
  sigaction(sig, &g_old[sig], nullptr);
  g_inCrash = 0;
  if (si->si_code <= 0) syscall(SYS_tgkill, getpid(), gettid(), sig);
}

void installCrashHandlers() {
  static char stack[64 * 1024];
  stack_t ss = {};
  ss.ss_sp = stack;
  ss.ss_size = sizeof stack;
  sigaltstack(&ss, nullptr);
  for (int s : kSigs) {
    struct sigaction sa = {};
    sa.sa_sigaction = crashHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(s, &sa, &g_old[s]) != 0) logf("sigaction(%d) failed: %s", s, strerror(errno));
  }
  logf("crash handlers installed; crash files go to %s", g_logdir.c_str());
}

// ------------------------------------------------------------------ environment report
void environmentReport() {
  logf("==== vkshim start ====");
  logf("package=%s intdir=%s logdir=%s nativeLibDir(shim dir)=%s", g_pkg.c_str(), g_intdir.c_str(),
       g_logdir.c_str(), g_nlib.c_str());
  const char* keys[] = {"ro.product.manufacturer", "ro.product.model", "ro.product.device", "ro.build.fingerprint",
                        "ro.build.version.release", "ro.build.version.sdk", "ro.build.version.security_patch",
                        "ro.board.platform", "ro.hardware", "ro.hardware.vulkan", "ro.hardware.egl",
                        "ro.soc.manufacturer", "ro.soc.model", "ro.product.cpu.abi", "ro.product.cpu.abilist",
                        "ro.vendor.build.fingerprint", "ro.opengles.version", "ro.surface_flinger.max_frame_buffer_acquired_buffers"};
  for (auto k : keys) logf("prop %s=%s", k, prop(k).c_str());
  utsname u;
  uname(&u);
  logf("uname: %s %s %s %s", u.sysname, u.release, u.version, u.machine);
  logf("selinux enforce: %s", readSmall("/sys/fs/selinux/enforce", 8).c_str());
  logf("loadavg: %s", readSmall("/proc/loadavg", 128).c_str());
  std::string mi = readSmall("/proc/meminfo", 400);
  logf("meminfo head: %s", mi.c_str());
  logf("shim self-check: pid=%d uid=%d", getpid(), getuid());
  logf("--- native lib dir listing");
  listDir(g_nlib);
  logf("--- required files");
  const char* need[] = {"libhook_impl.so", "libmain_hook.so", "libfile_redirect_hook.so", "liblinkernsbypass.so",
                        "libvulkan_purple.so", "vulkan.purple.so"};
  for (auto n : need) logf("  %-28s exists=%d size=%ld", n, (int)exists(g_nlib + "/" + n), fsize(g_nlib + "/" + n));
  logf("--- maps (game libs)");
  logMapsMatching("librdr", "libvkshim");
  logMapsMatching("libvulkan", "libNotlib");
}

// ------------------------------------------------------------------ hooks
PFN_vkGetInstanceProcAddr real_gipa;
PFN_vkGetDeviceProcAddr real_gdpa;
VkInstance g_inst = VK_NULL_HANDLE;
VkDevice g_dev = VK_NULL_HANDLE;
std::set<std::string> g_hideExt;       // HIDE_EXT=... in tu_env.txt
std::atomic<bool> g_lost{false};       // set when the first VK_ERROR_DEVICE_LOST is seen
std::atomic<long> g_nSubmit{0}, g_nPresent{0}, g_nAcquire{0}, g_nPipe{0}, g_nShader{0};
std::atomic<long> g_cDraw{0}, g_cBind{0}, g_cRP{0}, g_cDisp{0}, g_cCopy{0};  // command counts since the last submit
std::mutex g_nullm;
std::set<std::string> g_nullSeen;

PFN_vkVoidFunction resolveInst(const char* n) {
  void* p = g_vk ? dlsym(g_vk, n) : nullptr;
  if (!p && real_gipa) p = (void*)real_gipa(g_inst, n);
  return (PFN_vkVoidFunction)p;
}
void logNull(const char* api, const char* n) {
  std::lock_guard<std::mutex> lk(g_nullm);
  if (g_nullSeen.insert(std::string(api) + ":" + n).second && g_nullSeen.size() < 400)
    logf("PROCADDR NULL via %s: %s", api, n);
}
std::string pnextChain(const void* p) {
  std::string s;
  int guard = 0;
  while (p && guard++ < 64) {
    auto b = (const VkBaseInStructure*)p;
    char t[24];
    snprintf(t, sizeof t, "%d ", (int)b->sType);
    s += t;
    p = b->pNext;
  }
  return s;
}
const char* verStr(uint32_t v, char* b) {
  snprintf(b, 32, "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
  return b;
}

VKAPI_ATTR VkResult VKAPI_CALL h_EnumerateInstanceVersion(uint32_t* v) {
  static PFN_vkEnumerateInstanceVersion r;
  if (!r) r = (PFN_vkEnumerateInstanceVersion)resolveInst("vkEnumerateInstanceVersion");
  VkResult res = r ? r(v) : VK_ERROR_EXTENSION_NOT_PRESENT;
  char b[32];
  logf("vkEnumerateInstanceVersion -> res=%d version=%s", (int)res, (res == VK_SUCCESS && v) ? verStr(*v, b) : "?");
  return res;
}
VKAPI_ATTR VkResult VKAPI_CALL h_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a,
                                                VkInstance* out) {
  static PFN_vkCreateInstance r;
  if (!r) r = (PFN_vkCreateInstance)resolveInst("vkCreateInstance");
  char b[32];
  if (ci) {
    if (ci->pApplicationInfo)
      logf("vkCreateInstance: app='%s' engine='%s' apiVersion=%s", ci->pApplicationInfo->pApplicationName ? ci->pApplicationInfo->pApplicationName : "",
           ci->pApplicationInfo->pEngineName ? ci->pApplicationInfo->pEngineName : "",
           verStr(ci->pApplicationInfo->apiVersion, b));
    logf("vkCreateInstance: flags=0x%x pNext sTypes: %s", (unsigned)ci->flags, pnextChain(ci->pNext).c_str());
    for (uint32_t i = 0; i < ci->enabledLayerCount; i++) logf("  layer[%u]=%s", i, ci->ppEnabledLayerNames[i]);
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) logf("  inst-ext[%u]=%s", i, ci->ppEnabledExtensionNames[i]);
  }
  VkResult res = r ? r(ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
  if (res == VK_SUCCESS && out) g_inst = *out;
  logf("vkCreateInstance -> res=%d instance=%p", (int)res, out ? (void*)*out : nullptr);
  return res;
}
VKAPI_ATTR void VKAPI_CALL h_GetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties* p) {
  static PFN_vkGetPhysicalDeviceProperties r;
  if (!r) r = (PFN_vkGetPhysicalDeviceProperties)resolveInst("vkGetPhysicalDeviceProperties");
  if (r) r(pd, p);
  static int n = 0;
  if (p && n++ < 8) {
    char b[32];
    logf("vkGetPhysicalDeviceProperties: name='%s' apiVersion=%s driverVersion=0x%x vendor=0x%x device=0x%x type=%d",
         p->deviceName, verStr(p->apiVersion, b), p->driverVersion, p->vendorID, p->deviceID, (int)p->deviceType);
  }
}
VKAPI_ATTR void VKAPI_CALL h_GetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2* p) {
  static PFN_vkGetPhysicalDeviceProperties2 r;
  if (!r) r = (PFN_vkGetPhysicalDeviceProperties2)resolveInst("vkGetPhysicalDeviceProperties2");
  if (r) r(pd, p);
  static int n = 0;
  if (p && n++ < 8) {
    char b[32];
    logf("vkGetPhysicalDeviceProperties2: name='%s' apiVersion=%s pNext sTypes: %s", p->properties.deviceName,
         verStr(p->properties.apiVersion, b), pnextChain(p->pNext).c_str());
  }
}
const char* const kCoreFeat[55] = {
    "robustBufferAccess", "fullDrawIndexUint32", "imageCubeArray", "independentBlend", "geometryShader",
    "tessellationShader", "sampleRateShading", "dualSrcBlend", "logicOp", "multiDrawIndirect",
    "drawIndirectFirstInstance", "depthClamp", "depthBiasClamp", "fillModeNonSolid", "depthBounds", "wideLines",
    "largePoints", "alphaToOne", "multiViewport", "samplerAnisotropy", "textureCompressionETC2",
    "textureCompressionASTC_LDR", "textureCompressionBC", "occlusionQueryPrecise", "pipelineStatisticsQuery",
    "vertexPipelineStoresAndAtomics", "fragmentStoresAndAtomics", "shaderTessellationAndGeometryPointSize",
    "shaderImageGatherExtended", "shaderStorageImageExtendedFormats", "shaderStorageImageMultisample",
    "shaderStorageImageReadWithoutFormat", "shaderStorageImageWriteWithoutFormat",
    "shaderUniformBufferArrayDynamicIndexing", "shaderSampledImageArrayDynamicIndexing",
    "shaderStorageBufferArrayDynamicIndexing", "shaderStorageImageArrayDynamicIndexing", "shaderClipDistance",
    "shaderCullDistance", "shaderFloat64", "shaderInt64", "shaderInt16", "shaderResourceResidency",
    "shaderResourceMinLod", "sparseBinding", "sparseResidencyBuffer", "sparseResidencyImage2D",
    "sparseResidencyImage3D", "sparseResidency2Samples", "sparseResidency4Samples", "sparseResidency8Samples",
    "sparseResidency16Samples", "sparseResidencyAliased", "variableMultisampleRate", "inheritedQueries"};

const char* const kDescIdxFeat[20] = {
    "shaderInputAttachmentArrayDynamicIndexing", "shaderUniformTexelBufferArrayDynamicIndexing",
    "shaderStorageTexelBufferArrayDynamicIndexing", "shaderUniformBufferArrayNonUniformIndexing",
    "shaderSampledImageArrayNonUniformIndexing", "shaderStorageBufferArrayNonUniformIndexing",
    "shaderStorageImageArrayNonUniformIndexing", "shaderInputAttachmentArrayNonUniformIndexing",
    "shaderUniformTexelBufferArrayNonUniformIndexing", "shaderStorageTexelBufferArrayNonUniformIndexing",
    "descriptorBindingUniformBufferUpdateAfterBind", "descriptorBindingSampledImageUpdateAfterBind",
    "descriptorBindingStorageImageUpdateAfterBind", "descriptorBindingStorageBufferUpdateAfterBind",
    "descriptorBindingUniformTexelBufferUpdateAfterBind", "descriptorBindingStorageTexelBufferUpdateAfterBind",
    "descriptorBindingUpdateUnusedWhilePending", "descriptorBindingPartiallyBound",
    "descriptorBindingVariableDescriptorCount", "runtimeDescriptorArray"};

void logEnabledBools(const char* what, const VkBool32* b, const char* const* names, int n) {
  std::string on;
  for (int i = 0; i < n; i++)
    if (b[i]) { on += names[i]; on += ' '; }
  logf("  features[%s] enabled: %s", what, on.empty() ? "(none)" : on.c_str());
}

void logDeviceFeatures(const VkDeviceCreateInfo* ci) {
  if (ci->pEnabledFeatures) logEnabledBools("pEnabledFeatures", (const VkBool32*)ci->pEnabledFeatures, kCoreFeat, 55);
  const void* p = ci->pNext;
  int guard = 0;
  while (p && guard++ < 64) {
    auto b = (const VkBaseInStructure*)p;
    // the VkBool32 members of every VkPhysicalDevice*Features struct start right after sType/pNext (16 bytes)
    const VkBool32* f = (const VkBool32*)((const char*)p + sizeof(VkBaseInStructure));
    switch ((int)b->sType) {
      case 1000059000: logEnabledBools("Features2", f, kCoreFeat, 55); break;  // VkPhysicalDeviceFeatures2
      case 1000161001: logEnabledBools("DescriptorIndexing", f, kDescIdxFeat, 20); break;
      case 1000082000: { static const char* const n[] = {"shaderFloat16", "shaderInt8"}; logEnabledBools("Float16Int8", f, n, 2); break; }
      case 1000083000: { static const char* const n[] = {"storageBuffer16BitAccess", "uniformAndStorageBuffer16BitAccess", "storagePushConstant16", "storageInputOutput16"}; logEnabledBools("16BitStorage", f, n, 4); break; }
      case 1000267000: { static const char* const n[] = {"extendedDynamicState"}; logEnabledBools("ExtendedDynamicState", f, n, 1); break; }
      case 1000261000: { static const char* const n[] = {"hostQueryReset"}; logEnabledBools("HostQueryReset", f, n, 1); break; }
      case 1000108000: { static const char* const n[] = {"imagelessFramebuffer"}; logEnabledBools("ImagelessFramebuffer", f, n, 1); break; }
      default: break;
    }
    p = b->pNext;
  }
}

const char* vkres(int r) {
  switch (r) {
    case 0: return "SUCCESS";
    case 1: return "NOT_READY";
    case 2: return "TIMEOUT";
    case 3: return "EVENT_SET";
    case 4: return "EVENT_RESET";
    case 5: return "INCOMPLETE";
    case -1: return "ERROR_OUT_OF_HOST_MEMORY";
    case -2: return "ERROR_OUT_OF_DEVICE_MEMORY";
    case -3: return "ERROR_INITIALIZATION_FAILED";
    case -4: return "ERROR_DEVICE_LOST";
    case -5: return "ERROR_MEMORY_MAP_FAILED";
    case -6: return "ERROR_LAYER_NOT_PRESENT";
    case -7: return "ERROR_EXTENSION_NOT_PRESENT";
    case -8: return "ERROR_FEATURE_NOT_PRESENT";
    case -9: return "ERROR_INCOMPATIBLE_DRIVER";
    case -10: return "ERROR_TOO_MANY_OBJECTS";
    case -11: return "ERROR_FORMAT_NOT_SUPPORTED";
    case -12: return "ERROR_FRAGMENTED_POOL";
    case -13: return "ERROR_UNKNOWN";
    case -1000069000: return "ERROR_OUT_OF_POOL_MEMORY";
    case -1000000000: return "ERROR_SURFACE_LOST_KHR";
    case -1000000001: return "ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case 1000001003: return "SUBOPTIMAL_KHR";
    case -1000001004: return "ERROR_OUT_OF_DATE_KHR";
    default: return "(other)";
  }
}

// call after every wrapped call that returns a VkResult
void note(const char* fn, VkResult r) {
  if (r == VK_SUCCESS) return;
  if (r < 0 || r == VK_SUBOPTIMAL_KHR) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 300) logf("!! %s returned %s (%d)", fn, vkres((int)r), (int)r);
  }
  if (r == VK_ERROR_DEVICE_LOST && !g_lost.exchange(true)) {
    logf("*** FIRST VK_ERROR_DEVICE_LOST (GPU died) reported by %s - submits=%ld presents=%ld acquires=%ld pipelines=%ld shaders=%ld",
         fn, g_nSubmit.load(), g_nPresent.load(), g_nAcquire.load(), g_nPipe.load(), g_nShader.load());
    dumpRecent(120);
  }
}

template <class F>
F devFn(const char* name) {
  PFN_vkVoidFunction p = nullptr;
  if (real_gdpa && g_dev) p = real_gdpa(g_dev, name);
  if (!p && real_gipa) p = real_gipa(g_inst, name);
  return (F)p;
}

struct QEntry { VkQueue q; std::string name; };
std::vector<QEntry> g_queues;
std::mutex g_qm;
std::string qname(VkQueue q) {
  std::lock_guard<std::mutex> lk(g_qm);
  for (auto& e : g_queues) if (e.q == q) return e.name;
  return "q?";
}

VKAPI_ATTR VkResult VKAPI_CALL h_EnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char* layer, uint32_t* pCount,
                                                                    VkExtensionProperties* pProps) {
  static PFN_vkEnumerateDeviceExtensionProperties r;
  if (!r) r = (PFN_vkEnumerateDeviceExtensionProperties)resolveInst("vkEnumerateDeviceExtensionProperties");
  if (!r) return VK_ERROR_INITIALIZATION_FAILED;
  if (g_hideExt.empty()) return r(pd, layer, pCount, pProps);
  uint32_t n = 0;
  VkResult res = r(pd, layer, &n, nullptr);
  if (res != VK_SUCCESS) return res;
  std::vector<VkExtensionProperties> all(n);
  res = r(pd, layer, &n, all.data());
  if (res < 0) return res;
  all.resize(n);
  std::vector<VkExtensionProperties> keep;
  for (auto& e : all) {
    if (g_hideExt.count(e.extensionName)) { tracef("hiding device extension %s from the game", e.extensionName); continue; }
    keep.push_back(e);
  }
  if (!pProps) { *pCount = (uint32_t)keep.size(); return VK_SUCCESS; }
  uint32_t m = std::min<uint32_t>(*pCount, (uint32_t)keep.size());
  if (m) memcpy(pProps, keep.data(), m * sizeof(VkExtensionProperties));
  VkResult out = m < keep.size() ? VK_INCOMPLETE : VK_SUCCESS;
  *pCount = m;
  return out;
}

VKAPI_ATTR VkResult VKAPI_CALL h_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
                                              const VkAllocationCallbacks* a, VkDevice* out) {
  static PFN_vkCreateDevice r;
  if (!r) r = (PFN_vkCreateDevice)resolveInst("vkCreateDevice");
  VkDeviceCreateInfo ci2;
  std::vector<const char*> exts;
  const VkDeviceCreateInfo* use = ci;
  if (ci) {
    logf("vkCreateDevice: queues=%u pNext sTypes: %s", ci->queueCreateInfoCount, pnextChain(ci->pNext).c_str());
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) logf("  dev-ext[%u]=%s", i, ci->ppEnabledExtensionNames[i]);
    logDeviceFeatures(ci);
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++) {
      const VkDeviceQueueCreateInfo& qc = ci->pQueueCreateInfos[i];
      std::string pr;
      for (uint32_t j = 0; j < qc.queueCount && j < 8; j++) { char b[16]; snprintf(b, sizeof b, "%.2f ", qc.pQueuePriorities ? qc.pQueuePriorities[j] : 0.f); pr += b; }
      logf("  queue-create[%u]: family=%u queueCount=%u priorities=%s", i, qc.queueFamilyIndex, qc.queueCount, pr.c_str());
    }
    if (!g_hideExt.empty()) {
      for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        if (g_hideExt.count(ci->ppEnabledExtensionNames[i])) { logf("  (HIDE_EXT) removing %s from vkCreateDevice", ci->ppEnabledExtensionNames[i]); continue; }
        exts.push_back(ci->ppEnabledExtensionNames[i]);
      }
      ci2 = *ci;
      ci2.enabledExtensionCount = (uint32_t)exts.size();
      ci2.ppEnabledExtensionNames = exts.data();
      use = &ci2;
    }
  }
  VkResult res = r ? r(pd, use, a, out) : VK_ERROR_INITIALIZATION_FAILED;
  if (res == VK_SUCCESS && out) g_dev = *out;
  logf("vkCreateDevice -> res=%d device=%p", (int)res, out ? (void*)*out : nullptr);
  return res;
}

VKAPI_ATTR VkResult VKAPI_CALL h_CreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* a,
                                                    VkSwapchainKHR* out) {
  static PFN_vkCreateSwapchainKHR r;
  if (!r) r = devFn<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR");
  if (ci)
    logf("vkCreateSwapchainKHR: flags=0x%x minImages=%u format=%d colorSpace=%d extent=%ux%u layers=%u usage=0x%x sharing=%d "
         "preTransform=0x%x alpha=0x%x presentMode=%d clipped=%u old=%p pNext sTypes: %s",
         (unsigned)ci->flags, ci->minImageCount, (int)ci->imageFormat, (int)ci->imageColorSpace, ci->imageExtent.width,
         ci->imageExtent.height, ci->imageArrayLayers, (unsigned)ci->imageUsage, (int)ci->imageSharingMode,
         (unsigned)ci->preTransform, (unsigned)ci->compositeAlpha, (int)ci->presentMode, (unsigned)ci->clipped,
         (void*)ci->oldSwapchain, pnextChain(ci->pNext).c_str());
  VkResult res = r ? r(d, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
  logf("vkCreateSwapchainKHR -> %s (%d) swapchain=%p", vkres((int)res), (int)res, out ? (void*)*out : nullptr);
  note("vkCreateSwapchainKHR", res);
  return res;
}

VKAPI_ATTR VkResult VKAPI_CALL h_GetSwapchainImagesKHR(VkDevice d, VkSwapchainKHR sc, uint32_t* n, VkImage* imgs) {
  static PFN_vkGetSwapchainImagesKHR r;
  if (!r) r = devFn<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR");
  VkResult res = r ? r(d, sc, n, imgs) : VK_ERROR_INITIALIZATION_FAILED;
  static std::atomic<int> k{0};
  if (k.fetch_add(1) < 6) logf("vkGetSwapchainImagesKHR(%s) count=%u -> %s", imgs ? "images" : "count", n ? *n : 0, vkres((int)res));
  note("vkGetSwapchainImagesKHR", res);
  return res;
}

VKAPI_ATTR VkResult VKAPI_CALL h_AcquireNextImageKHR(VkDevice d, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, VkFence fence,
                                                     uint32_t* idx) {
  static PFN_vkAcquireNextImageKHR r;
  if (!r) r = devFn<PFN_vkAcquireNextImageKHR>("vkAcquireNextImageKHR");
  VkResult res = r ? r(d, sc, timeout, sem, fence, idx) : VK_ERROR_INITIALIZATION_FAILED;
  long n = ++g_nAcquire;
  tracef("acquire#%ld sc=%p -> %s idx=%u", n, (void*)sc, vkres((int)res), idx ? *idx : 0u);
  if (n <= 3) logf("vkAcquireNextImageKHR #%ld -> %s idx=%u", n, vkres((int)res), idx ? *idx : 0u);
  note("vkAcquireNextImageKHR", res);
  return res;
}

VKAPI_ATTR VkResult VKAPI_CALL h_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR* pi) {
  static PFN_vkQueuePresentKHR r;
  if (!r) r = devFn<PFN_vkQueuePresentKHR>("vkQueuePresentKHR");
  VkResult res = r ? r(q, pi) : VK_ERROR_INITIALIZATION_FAILED;
  long n = ++g_nPresent;
  tracef("present#%ld queue=%s waitSems=%u swapchains=%u -> %s", n, qname(q).c_str(), pi ? pi->waitSemaphoreCount : 0u, pi ? pi->swapchainCount : 0u, vkres((int)res));
  if (n <= 3) logf("vkQueuePresentKHR #%ld -> %s (pNext sTypes: %s)", n, vkres((int)res), pi ? pnextChain(pi->pNext).c_str() : "");
  if (pi && pi->pResults)
    for (uint32_t i = 0; i < pi->swapchainCount; i++) note("vkQueuePresentKHR(per-swapchain)", pi->pResults[i]);
  note("vkQueuePresentKHR", res);
  return res;
}

VKAPI_ATTR void VKAPI_CALL h_GetDeviceQueue(VkDevice d, uint32_t fam, uint32_t idx, VkQueue* out) {
  static PFN_vkGetDeviceQueue r;
  if (!r) r = devFn<PFN_vkGetDeviceQueue>("vkGetDeviceQueue");
  if (r) r(d, fam, idx, out);
  VkQueue q = out ? *out : nullptr;
  char nm[32];
  snprintf(nm, sizeof nm, "f%u/i%u", fam, idx);
  {
    std::lock_guard<std::mutex> lk(g_qm);
    bool found = false;
    for (auto& e : g_queues)
      if (e.q == q) { if (e.name.find(nm) == std::string::npos) e.name += std::string("+") + nm; found = true; }
    if (!found) g_queues.push_back({q, nm});
  }
  logf("vkGetDeviceQueue(family=%u, index=%u) -> queue %p", fam, idx, (void*)q);
}

void logQFam(const char* fn, uint32_t i, const VkQueueFamilyProperties& p) {
  logf("  %s family[%u]: flags=0x%x queueCount=%u timestampValidBits=%u", fn, i, (unsigned)p.queueFlags, p.queueCount, p.timestampValidBits);
}
VKAPI_ATTR void VKAPI_CALL h_GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice pd, uint32_t* n, VkQueueFamilyProperties* props) {
  static PFN_vkGetPhysicalDeviceQueueFamilyProperties r;
  if (!r) r = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)resolveInst("vkGetPhysicalDeviceQueueFamilyProperties");
  if (r) r(pd, n, props);
  static std::atomic<int> k{0};
  if (n && props && k.fetch_add(1) < 4) for (uint32_t i = 0; i < *n; i++) logQFam("vkGetPhysicalDeviceQueueFamilyProperties", i, props[i]);
}
void qfam2Impl(const char* name, VkPhysicalDevice pd, uint32_t* n, VkQueueFamilyProperties2* props) {
  static PFN_vkGetPhysicalDeviceQueueFamilyProperties2 r1, r2;
  PFN_vkGetPhysicalDeviceQueueFamilyProperties2& r = !strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties2") ? r1 : r2;
  if (!r) r = (PFN_vkGetPhysicalDeviceQueueFamilyProperties2)resolveInst(name);
  if (r) r(pd, n, props);
  static std::atomic<int> k{0};
  if (n && props && k.fetch_add(1) < 4) for (uint32_t i = 0; i < *n; i++) logQFam(name, i, props[i].queueFamilyProperties);
}
VKAPI_ATTR void VKAPI_CALL h_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice pd, uint32_t* n, VkQueueFamilyProperties2* p) { qfam2Impl("vkGetPhysicalDeviceQueueFamilyProperties2", pd, n, p); }
VKAPI_ATTR void VKAPI_CALL h_GetPhysicalDeviceQueueFamilyProperties2KHR(VkPhysicalDevice pd, uint32_t* n, VkQueueFamilyProperties2* p) { qfam2Impl("vkGetPhysicalDeviceQueueFamilyProperties2KHR", pd, n, p); }

// ---- command counters (cheap; reported at every submit so the log shows what kind of work was handed to the GPU)
VKAPI_ATTR void VKAPI_CALL h_CmdDraw(VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  static PFN_vkCmdDraw r;
  if (!r) r = devFn<PFN_vkCmdDraw>("vkCmdDraw");
  g_cDraw.fetch_add(1, std::memory_order_relaxed);
  if (r) r(cb, a, b, c, d);
}
VKAPI_ATTR void VKAPI_CALL h_CmdDrawIndexed(VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, int32_t d, uint32_t e) {
  static PFN_vkCmdDrawIndexed r;
  if (!r) r = devFn<PFN_vkCmdDrawIndexed>("vkCmdDrawIndexed");
  g_cDraw.fetch_add(1, std::memory_order_relaxed);
  if (r) r(cb, a, b, c, d, e);
}
VKAPI_ATTR void VKAPI_CALL h_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline pl) {
  static PFN_vkCmdBindPipeline r;
  if (!r) r = devFn<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");
  g_cBind.fetch_add(1, std::memory_order_relaxed);
  if (r) r(cb, bp, pl);
}
VKAPI_ATTR void VKAPI_CALL h_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi, VkSubpassContents sc) {
  static PFN_vkCmdBeginRenderPass r;
  if (!r) r = devFn<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass");
  g_cRP.fetch_add(1, std::memory_order_relaxed);
  if (r) r(cb, bi, sc);
}
VKAPI_ATTR void VKAPI_CALL h_CmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi, const VkSubpassBeginInfo* sb) {
  static PFN_vkCmdBeginRenderPass2 r;
  if (!r) r = devFn<PFN_vkCmdBeginRenderPass2>("vkCmdBeginRenderPass2");
  g_cRP.fetch_add(1, std::memory_order_relaxed);
  if (r) r(cb, bi, sb);
}
VKAPI_ATTR void VKAPI_CALL h_CmdBeginRenderPass2KHR(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi, const VkSubpassBeginInfo* sb) {
  static PFN_vkCmdBeginRenderPass2 r;
  if (!r) r = devFn<PFN_vkCmdBeginRenderPass2>("vkCmdBeginRenderPass2KHR");
  g_cRP.fetch_add(1, std::memory_order_relaxed);
  if (r) r(cb, bi, sb);
}
VKAPI_ATTR void VKAPI_CALL h_CmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
  static PFN_vkCmdDispatch r;
  if (!r) r = devFn<PFN_vkCmdDispatch>("vkCmdDispatch");
  g_cDisp.fetch_add(1, std::memory_order_relaxed);
  if (r) r(cb, x, y, z);
}
VKAPI_ATTR void VKAPI_CALL h_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n, const VkBufferImageCopy* rg) {
  static PFN_vkCmdCopyBufferToImage r;
  if (!r) r = devFn<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage");
  g_cCopy.fetch_add(1, std::memory_order_relaxed);
  if (r) r(cb, b, i, l, n, rg);
}

VKAPI_ATTR VkResult VKAPI_CALL h_QueueSubmit(VkQueue q, uint32_t cnt, const VkSubmitInfo* si, VkFence fence) {
  static PFN_vkQueueSubmit r;
  if (!r) r = devFn<PFN_vkQueueSubmit>("vkQueueSubmit");
  uint32_t cbs = 0, waits = 0, sigs = 0;
  bool hasNext = false;
  for (uint32_t i = 0; si && i < cnt; i++) {
    cbs += si[i].commandBufferCount;
    waits += si[i].waitSemaphoreCount;
    sigs += si[i].signalSemaphoreCount;
    if (si[i].pNext) hasNext = true;
  }
  long n = ++g_nSubmit;
  long cRP = g_cRP.exchange(0), cBind = g_cBind.exchange(0), cDraw = g_cDraw.exchange(0), cDisp = g_cDisp.exchange(0), cCopy = g_cCopy.exchange(0);
  std::string qn = qname(q);
  double t0 = nowSec();
  VkResult res = r ? r(q, cnt, si, fence) : VK_ERROR_INITIALIZATION_FAILED;
  double dt = nowSec() - t0;
  char what[160];
  snprintf(what, sizeof what, "renderpasses=%ld pipelineBinds=%ld draws=%ld dispatches=%ld bufToImageCopies=%ld", cRP, cBind, cDraw, cDisp, cCopy);
  tracef("submit#%ld queue=%s(%p) batches=%u cmdbufs=%u wait=%u signal=%u%s fence=%p -> %s (%.1f ms) | recorded since last submit: %s",
         n, qn.c_str(), (void*)q, cnt, cbs, waits, sigs, hasNext ? " +pNext" : "", (void*)fence, vkres((int)res), dt * 1000, what);
  if (n <= 14 || res != VK_SUCCESS)
    logf("vkQueueSubmit #%ld queue=%s batches=%u cmdbufs=%u wait=%u signal=%u fence=%s -> %s | %s", n, qn.c_str(), cnt, cbs, waits, sigs,
         fence ? "yes" : "no", vkres((int)res), what);
  if (dt > 0.25) logf("slow vkQueueSubmit #%ld took %.0f ms", n, dt * 1000);
  note("vkQueueSubmit", res);
  if (res == VK_SUCCESS && g_serialize > 0) {
    static PFN_vkQueueWaitIdle wi;
    static PFN_vkDeviceWaitIdle dw;
    if (!wi) wi = devFn<PFN_vkQueueWaitIdle>("vkQueueWaitIdle");
    if (!dw) dw = devFn<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle");
    double w0 = nowSec();
    VkResult wr = (g_serialize >= 2 && dw) ? dw(g_dev) : (wi ? wi(q) : VK_SUCCESS);
    double wd = nowSec() - w0;
    tracef("serialize: GPU finished submit#%ld on %s -> %s (%.1f ms)", n, qn.c_str(), vkres((int)wr), wd * 1000);
    if (n <= 40 || wr != VK_SUCCESS)
      logf("SERIALIZE: waited for GPU after submit #%ld (%s) -> %s in %.1f ms | %s", n, qn.c_str(), vkres((int)wr), wd * 1000, what);
    note("wait-after-submit", wr);
  }
  return res;
}

VkResult submit2Impl(const char* name, VkQueue q, uint32_t cnt, const VkSubmitInfo2* si, VkFence fence) {
  static PFN_vkQueueSubmit2 r1, r2;
  PFN_vkQueueSubmit2& r = !strcmp(name, "vkQueueSubmit2") ? r1 : r2;
  if (!r) r = devFn<PFN_vkQueueSubmit2>(name);
  long n = ++g_nSubmit;
  double t0 = nowSec();
  VkResult res = r ? r(q, cnt, si, fence) : VK_ERROR_INITIALIZATION_FAILED;
  double dt = nowSec() - t0;
  tracef("submit2#%ld queue=%p batches=%u fence=%p -> %s (%.1f ms)", n, (void*)q, cnt, (void*)fence, vkres((int)res), dt * 1000);
  if (n <= 3) logf("%s #%ld batches=%u -> %s", name, n, cnt, vkres((int)res));
  if (dt > 0.25) logf("slow %s #%ld took %.0f ms", name, n, dt * 1000);
  note(name, res);
  return res;
}
VKAPI_ATTR VkResult VKAPI_CALL h_QueueSubmit2(VkQueue q, uint32_t cnt, const VkSubmitInfo2* si, VkFence f) { return submit2Impl("vkQueueSubmit2", q, cnt, si, f); }
VKAPI_ATTR VkResult VKAPI_CALL h_QueueSubmit2KHR(VkQueue q, uint32_t cnt, const VkSubmitInfo2* si, VkFence f) { return submit2Impl("vkQueueSubmit2KHR", q, cnt, si, f); }

VKAPI_ATTR VkResult VKAPI_CALL h_WaitForFences(VkDevice d, uint32_t cnt, const VkFence* f, VkBool32 all, uint64_t timeout) {
  static PFN_vkWaitForFences r;
  if (!r) r = devFn<PFN_vkWaitForFences>("vkWaitForFences");
  double t0 = nowSec();
  VkResult res = r ? r(d, cnt, f, all, timeout) : VK_ERROR_INITIALIZATION_FAILED;
  double dt = nowSec() - t0;
  if (res != VK_SUCCESS || dt > 0.1) tracef("waitForFences n=%u all=%u timeout=%llu -> %s (%.1f ms)", cnt, (unsigned)all, (unsigned long long)timeout, vkres((int)res), dt * 1000);
  if (dt > 1.0) logf("slow vkWaitForFences: %.0f ms -> %s", dt * 1000, vkres((int)res));
  note("vkWaitForFences", res);
  return res;
}

VKAPI_ATTR VkResult VKAPI_CALL h_QueueWaitIdle(VkQueue q) {
  static PFN_vkQueueWaitIdle r;
  if (!r) r = devFn<PFN_vkQueueWaitIdle>("vkQueueWaitIdle");
  VkResult res = r ? r(q) : VK_ERROR_INITIALIZATION_FAILED;
  tracef("queueWaitIdle queue=%p -> %s", (void*)q, vkres((int)res));
  note("vkQueueWaitIdle", res);
  return res;
}
VKAPI_ATTR VkResult VKAPI_CALL h_DeviceWaitIdle(VkDevice d) {
  static PFN_vkDeviceWaitIdle r;
  if (!r) r = devFn<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle");
  VkResult res = r ? r(d) : VK_ERROR_INITIALIZATION_FAILED;
  tracef("deviceWaitIdle -> %s", vkres((int)res));
  note("vkDeviceWaitIdle", res);
  return res;
}

VKAPI_ATTR VkResult VKAPI_CALL h_CreateGraphicsPipelines(VkDevice d, VkPipelineCache pc, uint32_t cnt, const VkGraphicsPipelineCreateInfo* ci,
                                                         const VkAllocationCallbacks* a, VkPipeline* out) {
  static PFN_vkCreateGraphicsPipelines r;
  if (!r) r = devFn<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines");
  double t0 = nowSec();
  VkResult res = r ? r(d, pc, cnt, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
  double dt = nowSec() - t0;
  long n = (g_nPipe += cnt);
  tracef("createGraphicsPipelines n=%u total=%ld -> %s (%.1f ms)", cnt, n, vkres((int)res), dt * 1000);
  static std::atomic<int> k{0};
  int kk = k.fetch_add(1);
  if (kk < 3 || kk % 100 == 0) logf("vkCreateGraphicsPipelines call #%d n=%u (total %ld) -> %s %.0f ms", kk, cnt, n, vkres((int)res), dt * 1000);
  note("vkCreateGraphicsPipelines", res);
  return res;
}
VKAPI_ATTR VkResult VKAPI_CALL h_CreateComputePipelines(VkDevice d, VkPipelineCache pc, uint32_t cnt, const VkComputePipelineCreateInfo* ci,
                                                        const VkAllocationCallbacks* a, VkPipeline* out) {
  static PFN_vkCreateComputePipelines r;
  if (!r) r = devFn<PFN_vkCreateComputePipelines>("vkCreateComputePipelines");
  double t0 = nowSec();
  VkResult res = r ? r(d, pc, cnt, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
  double dt = nowSec() - t0;
  long n = (g_nPipe += cnt);
  tracef("createComputePipelines n=%u total=%ld -> %s (%.1f ms)", cnt, n, vkres((int)res), dt * 1000);
  static std::atomic<int> k{0};
  int kk = k.fetch_add(1);
  if (kk < 3 || kk % 100 == 0) logf("vkCreateComputePipelines call #%d n=%u (total %ld) -> %s %.0f ms", kk, cnt, n, vkres((int)res), dt * 1000);
  note("vkCreateComputePipelines", res);
  return res;
}
VKAPI_ATTR VkResult VKAPI_CALL h_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo* ci, const VkAllocationCallbacks* a,
                                                    VkShaderModule* out) {
  static PFN_vkCreateShaderModule r;
  if (!r) r = devFn<PFN_vkCreateShaderModule>("vkCreateShaderModule");
  VkResult res = r ? r(d, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
  long n = ++g_nShader;
  tracef("createShaderModule#%ld codeSize=%zu -> %s", n, ci ? (size_t)ci->codeSize : (size_t)0, vkres((int)res));
  note("vkCreateShaderModule", res);
  return res;
}
VKAPI_ATTR VkResult VKAPI_CALL h_AllocateMemory(VkDevice d, const VkMemoryAllocateInfo* ai, const VkAllocationCallbacks* a, VkDeviceMemory* out) {
  static PFN_vkAllocateMemory r;
  if (!r) r = devFn<PFN_vkAllocateMemory>("vkAllocateMemory");
  VkResult res = r ? r(d, ai, a, out) : VK_ERROR_INITIALIZATION_FAILED;
  if (res != VK_SUCCESS) logf("vkAllocateMemory FAILED size=%llu memoryTypeIndex=%u -> %s", ai ? (unsigned long long)ai->allocationSize : 0ULL, ai ? ai->memoryTypeIndex : 0u, vkres((int)res));
  else if (ai && ai->allocationSize >= (64ull << 20)) tracef("vkAllocateMemory size=%llu MB type=%u", (unsigned long long)(ai->allocationSize >> 20), ai->memoryTypeIndex);
  note("vkAllocateMemory", res);
  return res;
}

void* hookFor(const char* n) {
  if (!strcmp(n, "vkEnumerateInstanceVersion")) return (void*)h_EnumerateInstanceVersion;
  if (!strcmp(n, "vkCreateInstance")) return (void*)h_CreateInstance;
  if (!strcmp(n, "vkCreateDevice")) return (void*)h_CreateDevice;
  if (!strcmp(n, "vkGetPhysicalDeviceProperties")) return (void*)h_GetPhysicalDeviceProperties;
  if (!strcmp(n, "vkGetPhysicalDeviceProperties2") || !strcmp(n, "vkGetPhysicalDeviceProperties2KHR"))
    return (void*)h_GetPhysicalDeviceProperties2;
  if (!strcmp(n, "vkEnumerateDeviceExtensionProperties")) return (void*)h_EnumerateDeviceExtensionProperties;
  if (!strcmp(n, "vkCreateSwapchainKHR")) return (void*)h_CreateSwapchainKHR;
  if (!strcmp(n, "vkGetSwapchainImagesKHR")) return (void*)h_GetSwapchainImagesKHR;
  if (!strcmp(n, "vkAcquireNextImageKHR")) return (void*)h_AcquireNextImageKHR;
  if (!strcmp(n, "vkQueuePresentKHR")) return (void*)h_QueuePresentKHR;
  if (!strcmp(n, "vkQueueSubmit")) return (void*)h_QueueSubmit;
  if (!strcmp(n, "vkQueueSubmit2")) return (void*)h_QueueSubmit2;
  if (!strcmp(n, "vkQueueSubmit2KHR")) return (void*)h_QueueSubmit2KHR;
  if (!strcmp(n, "vkWaitForFences")) return (void*)h_WaitForFences;
  if (!strcmp(n, "vkQueueWaitIdle")) return (void*)h_QueueWaitIdle;
  if (!strcmp(n, "vkDeviceWaitIdle")) return (void*)h_DeviceWaitIdle;
  if (!strcmp(n, "vkCreateGraphicsPipelines")) return (void*)h_CreateGraphicsPipelines;
  if (!strcmp(n, "vkCreateComputePipelines")) return (void*)h_CreateComputePipelines;
  if (!strcmp(n, "vkCreateShaderModule")) return (void*)h_CreateShaderModule;
  if (!strcmp(n, "vkAllocateMemory")) return (void*)h_AllocateMemory;
  if (!strcmp(n, "vkGetDeviceQueue")) return (void*)h_GetDeviceQueue;
  if (!strcmp(n, "vkGetPhysicalDeviceQueueFamilyProperties")) return (void*)h_GetPhysicalDeviceQueueFamilyProperties;
  if (!strcmp(n, "vkGetPhysicalDeviceQueueFamilyProperties2")) return (void*)h_GetPhysicalDeviceQueueFamilyProperties2;
  if (!strcmp(n, "vkGetPhysicalDeviceQueueFamilyProperties2KHR")) return (void*)h_GetPhysicalDeviceQueueFamilyProperties2KHR;
  if (!strcmp(n, "vkCmdDraw")) return (void*)h_CmdDraw;
  if (!strcmp(n, "vkCmdDrawIndexed")) return (void*)h_CmdDrawIndexed;
  if (!strcmp(n, "vkCmdBindPipeline")) return (void*)h_CmdBindPipeline;
  if (!strcmp(n, "vkCmdBeginRenderPass")) return (void*)h_CmdBeginRenderPass;
  if (!strcmp(n, "vkCmdBeginRenderPass2")) return (void*)h_CmdBeginRenderPass2;
  if (!strcmp(n, "vkCmdBeginRenderPass2KHR")) return (void*)h_CmdBeginRenderPass2KHR;
  if (!strcmp(n, "vkCmdDispatch")) return (void*)h_CmdDispatch;
  if (!strcmp(n, "vkCmdCopyBufferToImage")) return (void*)h_CmdCopyBufferToImage;
  return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL h_GetInstanceProcAddr(VkInstance i, const char* n) {
  if (!n) return nullptr;
  if (!strcmp(n, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)h_GetInstanceProcAddr;
  if (void* h = hookFor(n)) return (PFN_vkVoidFunction)h;
  PFN_vkVoidFunction r = real_gipa ? real_gipa(i, n) : nullptr;
  if (!r) logNull("GIPA", n);
  return r;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL h_GetDeviceProcAddr(VkDevice d, const char* n) {
  if (!n) return nullptr;
  if (!strcmp(n, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)h_GetDeviceProcAddr;
  if (void* h = hookFor(n)) return (PFN_vkVoidFunction)h;
  PFN_vkVoidFunction r = real_gdpa ? real_gdpa(d, n) : nullptr;
  if (!r) logNull("GDPA", n);
  return r;
}
}  // namespace

namespace { void lazyInit(); std::once_flag g_once; bool g_missLogged[VKSHIM_COUNT]; }

// called from the asm slow path (tramp.S) the first time an exported vk* is used.
extern "C" HID void* vkshim_resolve(int idx) {
  std::call_once(g_once, lazyInit);
  void* p = (idx >= 0 && idx < VKSHIM_COUNT) ? vkshim_tbl[idx] : nullptr;
  if (!p) {
    if (idx >= 0 && idx < VKSHIM_COUNT && !g_missLogged[idx]) {
      g_missLogged[idx] = true;
      logf("CALLED UNRESOLVED EXPORT (returns 0): %s", kNames[idx]);
    }
    return (void*)vkshim_ret0;
  }
  return p;
}

namespace {
// ------------------------------------------------------------------ driver loading
bool copyFile(const std::string& from, const std::string& to) {
  int in = open(from.c_str(), O_RDONLY);
  if (in < 0) return false;
  int out = open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0700);
  if (out < 0) { close(in); return false; }
  char buf[1 << 16];
  ssize_t r;
  while ((r = read(in, buf, sizeof buf)) > 0) (void)!write(out, buf, r);
  close(in);
  close(out);
  return true;
}

void* loadVulkan() {
  std::string tmp = g_intdir + "/code_cache/vkshim_tmp";
  mkdirs(tmp);
  logf("tmpLibDir=%s exists=%d", tmp.c_str(), (int)exists(tmp));
  // driver can live next to the shim (preferred, packed into the APK as libvulkan_purple.so) ...
  std::string drvDir = g_nlib, drvName;
  const char* cand[] = {"libvulkan_purple.so", "vulkan.purple.so"};
  for (auto c : cand) if (exists(drvDir + "/" + c)) { drvName = c; break; }
  // ... or be dropped by hand into <logdir>/../driver/ and is then copied to private storage
  if (drvName.empty()) {
    std::string ext = g_logdir + "/../driver/vulkan.purple.so";
    if (exists(ext)) {
      std::string dst = g_intdir + "/code_cache/vkshim_driver";
      mkdirs(dst);
      logf("copying driver from %s to %s", ext.c_str(), dst.c_str());
      if (copyFile(ext, dst + "/vulkan.purple.so")) { drvDir = dst; drvName = "vulkan.purple.so"; }
    }
  }
  if (drvName.empty()) {
    logf("NO DRIVER FILE FOUND (looked for libvulkan_purple.so / vulkan.purple.so in %s and %s/../driver)", g_nlib.c_str(), g_logdir.c_str());
  } else {
    logf("driver: %s/%s size=%ld", drvDir.c_str(), drvName.c_str(), fsize(drvDir + "/" + drvName));
    logf("linkernsbypass_load_status()=%d (0 = adrenotools refuses to work)", (int)linkernsbypass_load_status());
    std::string redir = g_intdir + "/code_cache/vkshim_redirect";
    mkdirs(redir);
    struct Try { const char* name; int flags; const char* tmpd; const char* redir; } tries[] = {
        {"A: CUSTOM, tmp dir", ADRENOTOOLS_DRIVER_CUSTOM, tmp.c_str(), nullptr},
        {"B: CUSTOM, tmp=null", ADRENOTOOLS_DRIVER_CUSTOM, nullptr, nullptr},
        {"C: CUSTOM|FILE_REDIRECT", ADRENOTOOLS_DRIVER_CUSTOM | ADRENOTOOLS_DRIVER_FILE_REDIRECT, tmp.c_str(), redir.c_str()},
    };
    void* h = nullptr;
    for (auto& t : tries) {
      logf("try %s: hook=%s drvdir=%s drv=%s", t.name, g_nlib.c_str(), drvDir.c_str(), drvName.c_str());
      dlerror();
      h = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, t.flags, t.tmpd, (g_nlib + "/").c_str(),
                                     (drvDir + "/").c_str(), drvName.c_str(), t.redir, nullptr);
      const char* err = dlerror();
      logf("  -> handle=%p dlerror=%s", h, err ? err : "(none)");
      if (h) break;
    }
    const char* err = nullptr;
    logMapsMatching("purple", "linkernsbypass");
    if (h) { g_custom = true; return h; }
    logf("custom driver load FAILED (see dlerror above, and the 'required files' list at the top)");
  }
  dlerror();
  void* h = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  logf("FALLBACK dlopen(libvulkan.so) -> %p dlerror=%s", h, dlerror() ? "yes" : "(none)");
  return h;
}

void selfTest() {
  if (g_logdir.empty() || exists(g_logdir + "/NO_SELFTEST")) { logf("selftest skipped"); return; }
  logf("---- selftest: creating a throwaway Vulkan instance ----");
  auto ci = (PFN_vkCreateInstance)resolveInst("vkCreateInstance");
  auto eiv = (PFN_vkEnumerateInstanceVersion)resolveInst("vkEnumerateInstanceVersion");
  char b[32];
  if (eiv) { uint32_t v = 0; VkResult r = eiv(&v); logf("selftest instance version res=%d %s", (int)r, verStr(v, b)); }
  else logf("selftest: vkEnumerateInstanceVersion not found (loader is Vulkan 1.0)");
  if (!ci) { logf("selftest: vkCreateInstance missing"); return; }
  VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "vkshim-selftest", 1, "none", 1, VK_API_VERSION_1_1};
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &ai};
  VkInstance inst = VK_NULL_HANDLE;
  VkResult r = ci(&ici, nullptr, &inst);
  logf("selftest vkCreateInstance(1.1) -> %d", (int)r);
  if (r != VK_SUCCESS) return;
  auto gipa = real_gipa;
  auto enumPd = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
  auto getProps2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2");
  auto destroy = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
  uint32_t n = 0;
  if (enumPd && enumPd(inst, &n, nullptr) == VK_SUCCESS && n) {
    VkPhysicalDevice pds[4];
    if (n > 4) n = 4;
    enumPd(inst, &n, pds);
    for (uint32_t i = 0; i < n && getProps2; i++) {
      VkPhysicalDeviceDriverProperties drv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
      VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
      getProps2(pds[i], &p2);
      logf("selftest GPU[%u]: '%s' apiVersion=%s driverVersion=0x%x vendor=0x%x device=0x%x", i, p2.properties.deviceName,
           verStr(p2.properties.apiVersion, b), p2.properties.driverVersion, p2.properties.vendorID, p2.properties.deviceID);
      logf("selftest GPU[%u]: driverID=%d driverName='%s' driverInfo='%s' conformance=%u.%u.%u.%u", i, (int)drv.driverID,
           drv.driverName, drv.driverInfo, drv.conformanceVersion.major, drv.conformanceVersion.minor,
           drv.conformanceVersion.subminor, drv.conformanceVersion.patch);
    }
  } else logf("selftest: no physical devices (n=%u)", n);
  if (destroy) destroy(inst, nullptr);
  logf("---- selftest done ----");
}

// ------------------------------------------------------------------ tu_env.txt (editable settings, no rebuild needed)
const char* kEnvTemplate =
    "# vkshim settings (this file lives in Android/media/<package>/vkshim_logs/).\n"
    "# One KEY=VALUE per line. Lines starting with # are ignored.\n"
    "# Use only ONE TU_DEBUG line at a time - if there are several, the last one wins.\n"
    "# Changes take effect the next time the game starts - no rebuild needed.\n"
    "# Try ONE experiment at a time: remove the # in front of one line, start the game,\n"
    "# then look at vkshim.log. Flag names come from the Mesa Turnip driver; unknown ones are ignored.\n"
    "#\n"
    "# 1) draw frames straight into RAM (simpler, often more stable on weak Adreno chips)\n"
    "#TU_DEBUG=sysmem\n"
    "# 2) switch off an optimisation (LRZ) known to hang the GPU in some games\n"
    "#TU_DEBUG=nolrz\n"
    "# 3) switch off compressed framebuffers (UBWC)\n"
    "#TU_DEBUG=noubwc\n"
    "# 4) slow but strict: flush and wait after every draw, to find exactly where the GPU dies\n"
    "#TU_DEBUG=flushall,syncdraw\n"
    "# 5) flags can be combined with commas\n"
    "#TU_DEBUG=sysmem,nolrz,noubwc\n"
    "#\n"
    "# Hide device extensions from the game (comma separated). Example: stops the game's frame-pacing\n"
    "# library from using VK_GOOGLE_display_timing.\n"
    "#HIDE_EXT=VK_GOOGLE_display_timing\n"
    "#\n"
    "# DIAGNOSTIC: after every GPU submit, wait until the GPU has finished it. The log then shows\n"
    "# exactly which submit kills the GPU (1 = wait on that queue, 2 = wait on the whole device).\n"
    "#SERIALIZE_SUBMITS=1\n"
    "#\n"
    "# Write every submit/present/acquire call to vkshim.log (big log, only for deep debugging)\n"
    "#VKSHIM_VERBOSE=1\n";

std::string trimStr(std::string t) {
  size_t a = t.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = t.find_last_not_of(" \t\r\n");
  return t.substr(a, b - a + 1);
}

void loadEnvConfig() {
  if (g_logdir.empty()) return;
  std::string path = g_logdir + "/tu_env.txt";
  if (!exists(path)) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0660);
    if (fd >= 0) { (void)!write(fd, kEnvTemplate, strlen(kEnvTemplate)); close(fd); logf("created settings template %s", path.c_str()); }
  }
  std::string txt = readSmall(path.c_str(), 1 << 16);
  if (txt.rfind("<open failed", 0) == 0) { logf("settings file %s not readable: %s", path.c_str(), txt.c_str()); return; }
  int applied = 0;
  std::set<std::string> seenKeys;
  size_t pos = 0;
  while (pos <= txt.size()) {
    size_t nl = txt.find('\n', pos);
    std::string line = trimStr(txt.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
    pos = nl == std::string::npos ? txt.size() + 1 : nl + 1;
    if (line.empty() || line[0] == '#') continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos) { logf("settings: ignoring line without '=': %s", line.c_str()); continue; }
    std::string k = trimStr(line.substr(0, eq)), v = trimStr(line.substr(eq + 1));
    if (k.empty()) continue;
    if (!seenKeys.insert(k).second) logf("settings: WARNING %s appears more than once - the LAST one wins", k.c_str());
    if (k == "SERIALIZE_SUBMITS") {
      g_serialize = atoi(v.c_str());
      logf("settings: SERIALIZE_SUBMITS=%d", g_serialize);
    } else if (k == "HIDE_EXT") {
      size_t q = 0;
      while (q <= v.size()) {
        size_t c = v.find(',', q);
        std::string e = trimStr(v.substr(q, c == std::string::npos ? std::string::npos : c - q));
        q = c == std::string::npos ? v.size() + 1 : c + 1;
        if (!e.empty()) { g_hideExt.insert(e); logf("settings: hiding device extension %s", e.c_str()); }
      }
    } else if (k == "VKSHIM_VERBOSE") {
      g_verbose = v != "0";
      logf("settings: VKSHIM_VERBOSE=%d", (int)g_verbose);
    } else {
      setenv(k.c_str(), v.c_str(), 1);
      logf("settings: setenv %s=%s", k.c_str(), v.c_str());
    }
    applied++;
  }
  if (!applied) logf("settings: %s has no active lines (baseline run, driver defaults)", path.c_str());
  const char* t = getenv("TU_DEBUG");
  logf("settings: effective TU_DEBUG=%s", t ? t : "(unset)");
}

void lazyInit() {
  loadEnvConfig();
  logf("lazyInit: first Vulkan call from the game; loading driver now (linkernsbypass_load_status=%d)",
       (int)linkernsbypass_load_status());
  g_vk = loadVulkan();
  if (!g_vk) { logf("FATAL: no libvulkan handle at all"); return; }
  real_gipa = (PFN_vkGetInstanceProcAddr)dlsym(g_vk, "vkGetInstanceProcAddr");
  real_gdpa = (PFN_vkGetDeviceProcAddr)dlsym(g_vk, "vkGetDeviceProcAddr");
  logf("real vkGetInstanceProcAddr=%p vkGetDeviceProcAddr=%p", (void*)real_gipa, (void*)real_gdpa);
  int ok = 0, hooked = 0, miss = 0;
  for (int i = 0; i < VKSHIM_COUNT; i++) {
    void* h = hookFor(kNames[i]);
    if (!strcmp(kNames[i], "vkGetInstanceProcAddr")) h = (void*)h_GetInstanceProcAddr;
    if (!strcmp(kNames[i], "vkGetDeviceProcAddr")) h = (void*)h_GetDeviceProcAddr;
    if (h) { vkshim_tbl[i] = h; hooked++; continue; }
    void* p = dlsym(g_vk, kNames[i]);
    if (p) { vkshim_tbl[i] = p; ok++; } else miss++;
  }
  logf("export table: %d forwarded, %d hooked, %d not exported by loader (logged as UNRESOLVED only if the game calls them directly; most are fetched via vkGet*ProcAddr)", ok, hooked, miss);
  selfTest();
  logf("==== vkshim ready (custom_driver=%d) ====", (int)g_custom);
}

__attribute__((constructor)) void shimInit() {
  initPaths();
  installCrashHandlers();
  logf("vkshim build: v4-diag (logs in Android/media, queue + submit detail, command counters, SERIALIZE_SUBMITS)");
  environmentReport();
  {  // safety net: normally libdatalink.so (loaded from Java) already did this; running again is a no-op
    datalink::Ctx dc;
    if (datalink::detect(dc)) logf("datalink (from shim): %d folder(s) linked, see Android/media/%s/datalink.log", datalink::run(dc), g_pkg.c_str());
  }
  logf("constructor done; driver load is deferred to the first vk call so libadrenotools' own static init has run");
}
}  // namespace
