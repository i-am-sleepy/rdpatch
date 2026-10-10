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

#include <mutex>
#include <set>
#include <string>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <adrenotools/driver.h>

#define HID __attribute__((visibility("hidden")))
#define EXP __attribute__((visibility("default")))

#include "names.inc"

extern "C" {
HID void* vkshim_tbl[VKSHIM_COUNT];
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
  const char* ext[] = {"/storage/emulated/0/Android/data/", "/sdcard/Android/data/", nullptr};
  for (int i = 0; ext[i]; i++) {
    std::string d = std::string(ext[i]) + g_pkg + "/files/vkshim_logs";
    mkdirs(d);
    if (exists(d)) { g_logdir = d; break; }
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
VKAPI_ATTR VkResult VKAPI_CALL h_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
                                              const VkAllocationCallbacks* a, VkDevice* out) {
  static PFN_vkCreateDevice r;
  if (!r) r = (PFN_vkCreateDevice)resolveInst("vkCreateDevice");
  if (ci) {
    logf("vkCreateDevice: queues=%u pNext sTypes: %s", ci->queueCreateInfoCount, pnextChain(ci->pNext).c_str());
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) logf("  dev-ext[%u]=%s", i, ci->ppEnabledExtensionNames[i]);
  }
  VkResult res = r ? r(pd, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
  logf("vkCreateDevice -> res=%d device=%p", (int)res, out ? (void*)*out : nullptr);
  return res;
}

void* hookFor(const char* n) {
  if (!strcmp(n, "vkEnumerateInstanceVersion")) return (void*)h_EnumerateInstanceVersion;
  if (!strcmp(n, "vkCreateInstance")) return (void*)h_CreateInstance;
  if (!strcmp(n, "vkCreateDevice")) return (void*)h_CreateDevice;
  if (!strcmp(n, "vkGetPhysicalDeviceProperties")) return (void*)h_GetPhysicalDeviceProperties;
  if (!strcmp(n, "vkGetPhysicalDeviceProperties2") || !strcmp(n, "vkGetPhysicalDeviceProperties2KHR"))
    return (void*)h_GetPhysicalDeviceProperties2;
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
  PFN_vkVoidFunction r = real_gdpa ? real_gdpa(d, n) : nullptr;
  if (!r) logNull("GDPA", n);
  return r;
}
}  // namespace

extern "C" HID unsigned long vkshim_missing(int idx) {
  logf("CALLED UNRESOLVED EXPORT: %s", (idx >= 0 && idx < VKSHIM_COUNT) ? kNames[idx] : "?");
  return 0;
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
  std::string tmp = g_intdir + "/files/vkshim_tmp";
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
      std::string dst = g_intdir + "/files/vkshim_driver";
      mkdirs(dst);
      logf("copying driver from %s to %s", ext.c_str(), dst.c_str());
      if (copyFile(ext, dst + "/vulkan.purple.so")) { drvDir = dst; drvName = "vulkan.purple.so"; }
    }
  }
  if (drvName.empty()) {
    logf("NO DRIVER FILE FOUND (looked for libvulkan_purple.so / vulkan.purple.so in %s and %s/../driver)", g_nlib.c_str(), g_logdir.c_str());
  } else {
    logf("driver: %s/%s size=%ld", drvDir.c_str(), drvName.c_str(), fsize(drvDir + "/" + drvName));
    logf("calling adrenotools_open_libvulkan(flags=RTLD_NOW|RTLD_LOCAL feature=CUSTOM tmp=%s hook=%s drvdir=%s drv=%s)",
         tmp.c_str(), g_nlib.c_str(), drvDir.c_str(), drvName.c_str());
    dlerror();
    void* h = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM, tmp.c_str(), (g_nlib + "/").c_str(),
                                         (drvDir + "/").c_str(), drvName.c_str(), nullptr, nullptr);
    const char* err = dlerror();
    logf("adrenotools_open_libvulkan -> handle=%p dlerror=%s", h, err ? err : "(none)");
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

__attribute__((constructor)) void shimInit() {
  initPaths();
  installCrashHandlers();
  environmentReport();
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
  logf("export table: %d forwarded, %d hooked, %d not exported by loader (calls to those are logged as UNRESOLVED; games normally fetch them via vkGet*ProcAddr)", ok, hooked, miss);
  selfTest();
  logf("==== vkshim ready (custom_driver=%d) ====", (int)g_custom);
}
}  // namespace
