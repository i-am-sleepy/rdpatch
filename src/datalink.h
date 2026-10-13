// datalink: makes the app's protected data folders live in /sdcard/Android/media/<pkg>/ instead.
// For each chosen folder  /data/user/0/<pkg>/<name>  it leaves a symlink pointing to
// /storage/emulated/0/Android/media/<pkg>/<name>  (the real files live there, editable with any file manager).
// Header-only so it can be used by libdatalink.so and by libvkshim.so (idempotent: second run is a no-op).
#pragma once
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace datalink {

struct Ctx {
  std::string pkg, intdir, mediadir;
  FILE* log = nullptr;
  long long maxBytes = 1536LL << 20;  // dirs bigger than this are not migrated (cfg: maxmb N)
};

inline void L(Ctx& c, const char* fmt, ...) {
  char b[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (c.log) { fprintf(c.log, "%s\n", b); fflush(c.log); }
#ifdef __ANDROID__
  __android_log_write(ANDROID_LOG_INFO, "datalink", b);
#endif
}

inline bool isLink(const std::string& p) { struct stat st; return lstat(p.c_str(), &st) == 0 && S_ISLNK(st.st_mode); }
inline bool isDir(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
inline bool existsL(const std::string& p) { struct stat st; return lstat(p.c_str(), &st) == 0; }

inline bool mkdirs(const std::string& p) {
  std::string cur;
  for (size_t i = 0; i < p.size(); i++) {
    cur += p[i];
    if ((p[i] == '/' || i + 1 == p.size()) && cur.size() > 1) {
      if (mkdir(cur.c_str(), 0771) != 0 && errno != EEXIST) return false;
    }
  }
  return isDir(p);
}

inline long long dirSize(const std::string& p) {  // does not follow symlinks
  long long total = 0;
  DIR* d = opendir(p.c_str());
  if (!d) return 0;
  while (dirent* e = readdir(d)) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    std::string q = p + "/" + e->d_name;
    struct stat st;
    if (lstat(q.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) total += dirSize(q);
    else if (S_ISREG(st.st_mode)) total += st.st_size;
  }
  closedir(d);
  return total;
}

inline bool copyFile(const std::string& from, const std::string& to) {
  int in = open(from.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) return false;
  int out = open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
  if (out < 0) { close(in); return false; }
  static char buf[1 << 16];
  bool ok = true;
  ssize_t r;
  while ((r = read(in, buf, sizeof buf)) > 0) {
    const char* p = buf;
    while (r > 0) { ssize_t w = write(out, p, r); if (w <= 0) { ok = false; break; } p += w; r -= w; }
    if (!ok) break;
  }
  if (r < 0) ok = false;
  close(in);
  close(out);
  return ok;
}

// copies everything from src into dst; files that already exist in dst are NOT overwritten (dst wins).
inline bool copyTree(Ctx& c, const std::string& src, const std::string& dst, int& files, int& fails) {
  if (!mkdirs(dst)) { L(c, "  mkdir failed: %s (%s)", dst.c_str(), strerror(errno)); fails++; return false; }
  DIR* d = opendir(src.c_str());
  if (!d) return false;
  bool ok = true;
  while (dirent* e = readdir(d)) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    std::string s = src + "/" + e->d_name, t = dst + "/" + e->d_name;
    struct stat st;
    if (lstat(s.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) { if (!copyTree(c, s, t, files, fails)) ok = false; }
    else if (S_ISREG(st.st_mode)) {
      if (existsL(t)) continue;
      if (copyFile(s, t)) files++;
      else { L(c, "  copy failed: %s -> %s (%s)", s.c_str(), t.c_str(), strerror(errno)); fails++; ok = false; }
    }  // symlinks / sockets / fifos are ignored
  }
  closedir(d);
  return ok;
}

inline void rmTree(const std::string& p) {
  struct stat st;
  if (lstat(p.c_str(), &st) != 0) return;
  if (S_ISDIR(st.st_mode)) {
    if (DIR* d = opendir(p.c_str())) {
      while (dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        rmTree(p + "/" + e->d_name);
      }
      closedir(d);
    }
    rmdir(p.c_str());
  } else unlink(p.c_str());
}

inline std::vector<std::string> listNames(const std::string& p) {
  std::vector<std::string> v;
  if (DIR* d = opendir(p.c_str())) {
    while (dirent* e = readdir(d)) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) v.push_back(e->d_name);
    closedir(d);
  }
  std::sort(v.begin(), v.end());
  return v;
}

inline void tree(Ctx& c, const std::string& p, int depth, int maxDepth, int& budget, const std::string& indent) {
  for (auto& n : listNames(p)) {
    if (budget-- <= 0) { L(c, "%s... (truncated)", indent.c_str()); return; }
    std::string q = p + "/" + n;
    struct stat st;
    if (lstat(q.c_str(), &st) != 0) continue;
    if (S_ISLNK(st.st_mode)) {
      char t[512] = {0};
      ssize_t r = readlink(q.c_str(), t, sizeof t - 1);
      L(c, "%s%s -> %s", indent.c_str(), n.c_str(), r > 0 ? t : "?");
      if (depth < maxDepth && isDir(q)) tree(c, q, depth + 1, maxDepth, budget, indent + "    ");
    } else if (S_ISDIR(st.st_mode)) {
      L(c, "%s%s/", indent.c_str(), n.c_str());
      if (depth < maxDepth) tree(c, q, depth + 1, maxDepth, budget, indent + "    ");
    } else
      L(c, "%s%s  (%lld bytes)", indent.c_str(), n.c_str(), (long long)st.st_size);
  }
}

inline std::vector<std::string> readLines(const std::string& p) {
  std::vector<std::string> v;
  FILE* f = fopen(p.c_str(), "r");
  if (!f) return v;
  char b[512];
  while (fgets(b, sizeof b, f)) {
    std::string s = b;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    s = s.substr(i);
    if (!s.empty() && s[0] != '#') v.push_back(s);
  }
  fclose(f);
  return v;
}

static const char* kDefaultCfg =
    "# datalink.cfg - edit, then restart the game (changes apply on next launch)\n"
    "# Everything under /data/data/<pkg> is moved to this media folder, except what is excluded.\n"
    "# Lines:  include <path> | exclude <pattern> | maxmb <N> | off\n"
    "#   paths are relative to /data/data/<pkg>, e.g.  files/options.xml   (alias: includefile / file)\n"
    "#   exclude takes * wildcards, e.g.  exclude files/assetpacks   exclude files/*.tmp\n"
    "# A folder is linked as a whole when it is smaller than maxmb and has no excluded content.\n"
    "# Otherwise it stays a real folder and its files/subfolders are linked one by one (big or excluded parts stay put).\n"
    "# New files the game creates are moved here on the next launch.\n"
    "# 'off' stops any new linking (existing links stay; to undo, clear app data or delete the symlinks)\n"
    "include files\n"
    "exclude cache\n"
    "exclude code_cache\n"
    "exclude lib\n"
    "exclude no_backup\n"
    "exclude databases\n"
    "exclude files/assetpacks\n"
    "maxmb 1536\n";

inline std::string dirOf(const std::string& p) { size_t s = p.rfind('/'); return s == std::string::npos ? "." : p.substr(0, s); }

inline bool matchExcluded(const std::vector<std::string>& pats, const std::string& rel) {
  size_t pos = 0;
  while (true) {  // rel itself and every parent prefix
    size_t sl = rel.find('/', pos);
    std::string pre = sl == std::string::npos ? rel : rel.substr(0, sl);
    for (auto& p : pats) if (fnmatch(p.c_str(), pre.c_str(), 0) == 0) return true;
    if (sl == std::string::npos) return false;
    pos = sl + 1;
  }
}
inline bool patternUnder(const std::vector<std::string>& pats, const std::string& rel) {
  std::string pre = rel + "/";
  for (auto& p : pats) if (p.compare(0, pre.size(), pre) == 0) return true;
  return false;
}

struct State {
  std::vector<std::string> exc;  // from datalink.cfg (a folder that contains such paths is never linked as a whole)
  std::vector<std::string> bi;   // built-in excludes
  std::set<std::string> inc;
  int links = 0;
};

inline void handleEntry(Ctx& c, State& st, const std::string& rel, bool top);

inline bool verifyCopy(const std::string& a, const std::string& b) {
  struct stat sa, sb;
  return stat(a.c_str(), &sa) == 0 && stat(b.c_str(), &sb) == 0 && sa.st_size == sb.st_size;
}
inline void copyMtime(const std::string& from, const std::string& to) {
  struct stat sa;
  if (stat(from.c_str(), &sa) != 0) return;
  timespec ts[2] = {sa.st_atim, sa.st_mtim};
  utimensat(AT_FDCWD, to.c_str(), ts, 0);  // best effort
}

inline void linkFile(Ctx& c, State& st, const std::string& rel, const std::string& src, const std::string& dst, bool hasS, bool hasD) {
  if (hasS) {
    struct stat ss;
    lstat(src.c_str(), &ss);
    bool doCopy = !hasD;
    if (hasD) {
      struct stat sd;
      stat(dst.c_str(), &sd);
      if (ss.st_mtime > sd.st_mtime) { doCopy = true; L(c, "[%s] data copy is newer than media copy (game replaced the link?) -> media updated", rel.c_str()); }
      else L(c, "[%s] media copy is newer or equal -> media copy wins", rel.c_str());
    }
    if (doCopy) {
      if (!mkdirs(dirOf(dst))) { L(c, "[%s] cannot create %s (%s)", rel.c_str(), dirOf(dst).c_str(), strerror(errno)); return; }
      if (!copyFile(src, dst) || !verifyCopy(src, dst)) { L(c, "[%s] copy failed (%s), original untouched", rel.c_str(), strerror(errno)); return; }
      copyMtime(src, dst);
    }
    std::string bak = c.intdir + "/datalink_bak/" + rel;
    mkdirs(dirOf(bak));
    unlink(bak.c_str());
    if (rename(src.c_str(), bak.c_str()) != 0) { L(c, "[%s] backup rename failed (%s)", rel.c_str(), strerror(errno)); return; }
  } else {
    mkdirs(dirOf(src));
  }
  if (symlink(dst.c_str(), src.c_str()) != 0) {
    L(c, "[%s] symlink failed (%s)", rel.c_str(), strerror(errno));
    if (hasS) rename((c.intdir + "/datalink_bak/" + rel).c_str(), src.c_str());
    return;
  }
  L(c, "[%s] file linked", rel.c_str());
  st.links++;
}

inline void linkDirWhole(Ctx& c, State& st, const std::string& rel, const std::string& src, const std::string& dst, long long sz) {
  if (!mkdirs(dst)) { L(c, "[%s] cannot create %s (%s) - skipped", rel.c_str(), dst.c_str(), strerror(errno)); return; }
  L(c, "[%s] folder linked as a whole, %lld bytes", rel.c_str(), sz);
  int files = 0, fails = 0;
  copyTree(c, src, dst, files, fails);
  L(c, "[%s] copied %d files, %d failures", rel.c_str(), files, fails);
  if (fails) { L(c, "[%s] copy had failures, original left untouched", rel.c_str()); return; }
  std::string bak = src + ".datalink_bak";
  rmTree(bak);
  if (rename(src.c_str(), bak.c_str()) != 0) { L(c, "[%s] rename to backup failed (%s)", rel.c_str(), strerror(errno)); return; }
  if (symlink(dst.c_str(), src.c_str()) != 0) {
    L(c, "[%s] symlink failed (%s), restoring", rel.c_str(), strerror(errno));
    rename(bak.c_str(), src.c_str());
    return;
  }
  L(c, "[%s] linked (old copy kept as %s)", rel.c_str(), bak.c_str());
  st.links++;
}

inline void handleEntry(Ctx& c, State& st, const std::string& rel, bool top) {
  if (matchExcluded(st.exc, rel) || matchExcluded(st.bi, rel)) { if (top || std::count(rel.begin(), rel.end(), '/') < 2) L(c, "[%s] excluded", rel.c_str()); return; }
  std::string src = c.intdir + "/" + rel, dst = c.mediadir + "/" + rel;
  struct stat ls;
  bool hasS = lstat(src.c_str(), &ls) == 0;
  if (hasS && S_ISLNK(ls.st_mode)) {
    char t[512] = {0};
    ssize_t r = readlink(src.c_str(), t, sizeof t - 1);
    if (r > 0 && dst == t) { L(c, "[%s] already linked", rel.c_str()); st.links++; }
    else L(c, "[%s] is a symlink to something else (%s), left alone", rel.c_str(), t);
    return;
  }
  struct stat ds;
  bool hasD = stat(dst.c_str(), &ds) == 0;
  bool srcDir = hasS && S_ISDIR(ls.st_mode), srcReg = hasS && S_ISREG(ls.st_mode);
  bool dstDir = hasD && S_ISDIR(ds.st_mode), dstReg = hasD && S_ISREG(ds.st_mode);
  if (srcDir || (!hasS && dstDir)) {
    if (!hasS) {  // folder exists only in the media folder: make the app see it
      if (top && !st.inc.count(rel)) return;
      mkdirs(dirOf(src));
      if (symlink(dst.c_str(), src.c_str()) == 0) { L(c, "[%s] media-only folder linked", rel.c_str()); st.links++; }
      return;
    }
    long long sz = dirSize(src);
    bool under = patternUnder(st.exc, rel);
    if (sz <= c.maxBytes && !under) { linkDirWhole(c, st, rel, src, dst, sz); return; }
    L(c, "[%s] kept as a real folder (%s); linking its contents one by one", rel.c_str(),
      under ? "contains excluded paths" : "bigger than maxmb");
    mkdirs(dst);
    std::set<std::string> names;
    for (auto& n : listNames(src)) names.insert(n);
    if (dstDir) for (auto& n : listNames(dst)) names.insert(n);
    for (auto& n : names) handleEntry(c, st, rel + "/" + n, false);
    return;
  }
  if (srcReg || (!hasS && dstReg)) {
    if (top && !st.inc.count(rel)) { L(c, "[%s] plain file at top level, skipped (use 'include %s')", rel.c_str(), rel.c_str()); return; }
    linkFile(c, st, rel, src, dst, srcReg, dstReg);
  }
}

// returns number of entries that are now linked
inline int run(Ctx& c) {
  if (!mkdirs(c.mediadir)) {
    fprintf(stderr, "datalink: cannot create %s (%s)\n", c.mediadir.c_str(), strerror(errno));
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_ERROR, "datalink", "cannot create %s: %s", c.mediadir.c_str(), strerror(errno));
#endif
    return -1;
  }
  std::string logp = c.mediadir + "/datalink.log";
  struct stat lst;
  bool big = stat(logp.c_str(), &lst) == 0 && lst.st_size > (256 << 10);
  c.log = fopen(logp.c_str(), big ? "w" : "a");  // append: the first run's migration details must not be overwritten
  if (c.log) fprintf(c.log, "\n===== %ld =====\n", (long)time(nullptr));
  L(c, "datalink run: pkg=%s intdir=%s mediadir=%s", c.pkg.c_str(), c.intdir.c_str(), c.mediadir.c_str());
  std::string cfg = c.mediadir + "/datalink.cfg";
  if (!existsL(cfg)) {
    if (FILE* f = fopen(cfg.c_str(), "w")) { fputs(kDefaultCfg, f); fclose(f); L(c, "wrote default %s", cfg.c_str()); }
  }
  State st;
  // built-in excludes: our own scratch folders and androidx profile-installer markers
  for (const char* b : {"*vkshim_*", "files/profileInstalled", "files/profileinstaller_*", "datalink_bak", "*.datalink_bak"}) st.bi.push_back(b);
  bool off = false;
  for (auto& line : readLines(cfg)) {
    char a[20] = {0}, b[400] = {0};
    int n = sscanf(line.c_str(), "%19s %399s", a, b);
    if (n >= 1 && !strcmp(a, "off")) off = true;
    else if (n == 2 && (!strcmp(a, "include") || !strcmp(a, "includefile") || !strcmp(a, "file"))) st.inc.insert(b);
    else if (n == 2 && !strcmp(a, "exclude")) st.exc.push_back(b);
    else if (n == 2 && !strcmp(a, "maxmb")) c.maxBytes = atoll(b) << 20;
    else L(c, "cfg: ignored line '%s'", line.c_str());
  }
  if (off) { L(c, "cfg says 'off' - not linking anything"); if (c.log) fclose(c.log); c.log = nullptr; return 0; }

  std::set<std::string> top;
  for (auto& n : listNames(c.intdir)) top.insert(n);
  for (auto& i : st.inc) if (i.find('/') == std::string::npos) top.insert(i);
  for (auto& name : top) {
    if (name.find('/') != std::string::npos || name == "." || name == "..") continue;
    handleEntry(c, st, name, true);
  }
  for (auto& i : st.inc) {  // explicit deep includes (e.g. files/graphics.xml), only if no parent is a symlink
    if (i.find('/') == std::string::npos) continue;
    bool parentLink = false;
    size_t pos = 0;
    while ((pos = i.find('/', pos)) != std::string::npos) { if (isLink(c.intdir + "/" + i.substr(0, pos))) parentLink = true; pos++; }
    if (!parentLink) handleEntry(c, st, i, false);
  }
  L(c, "---- layout of %s (depth 3) ----", c.intdir.c_str());
  int budget = 400;
  tree(c, c.intdir, 1, 3, budget, "  ");
  L(c, "datalink done: %d entr%s linked", st.links, st.links == 1 ? "y" : "ies");
  if (c.log) { fclose(c.log); c.log = nullptr; }
  return st.links;
}

inline bool detect(Ctx& c) {  // fills pkg/intdir/mediadir from the running process; false if not the main process
  char cl[256] = {0};
  int fd = open("/proc/self/cmdline", O_RDONLY);
  if (fd < 0) return false;
  (void)!read(fd, cl, sizeof cl - 1);
  close(fd);
  c.pkg = cl;
  if (c.pkg.empty() || c.pkg.find(':') != std::string::npos || c.pkg.find('/') != std::string::npos) return false;
  for (const char* base : {"/data/user/0/", "/data/data/"}) {
    if (isDir(std::string(base) + c.pkg)) { c.intdir = std::string(base) + c.pkg; break; }
  }
  if (c.intdir.empty()) return false;
  c.mediadir = "/storage/emulated/0/Android/media/" + c.pkg;
  return true;
}

}  // namespace datalink
