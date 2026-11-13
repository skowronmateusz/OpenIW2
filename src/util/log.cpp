// Session logs, event lines and the trace recorder (docs/logging.md). An openiw2 addition.
#include "util/log.h"

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#endif
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define IW2_HAVE_BACKTRACE 1
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

extern char** environ;

namespace iw2::log {

namespace {

namespace stdfs = std::filesystem;

bool iequalsPrefix(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
    return true;
}
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

void writeAll(int fd, const char* p, size_t n) {
    while (n > 0) {
        const ssize_t w = ::write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return;
        p += w;
        n -= size_t(w);
    }
}
void writeStr(int fd, const char* s) { writeAll(fd, s, std::strlen(s)); }

int64_t monoMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// "[mm:ss.mmm]" of an elapsed time, written into buf (at least 24 bytes) without the C library (signal handler).
size_t formatElapsed(char* buf, int64_t ms) {
    if (ms < 0) ms = 0;
    const int64_t mins = ms / 60000, secs = (ms / 1000) % 60, milli = ms % 1000;
    char digits[24];
    size_t n = 0, k = 0;
    int64_t m = mins;
    do { digits[k++] = char('0' + m % 10); m /= 10; } while (m > 0 && k < 20);
    if (k < 2) digits[k++] = '0';
    buf[n++] = '[';
    while (k > 0) buf[n++] = digits[--k];
    buf[n++] = ':';
    buf[n++] = char('0' + secs / 10);
    buf[n++] = char('0' + secs % 10);
    buf[n++] = '.';
    buf[n++] = char('0' + milli / 100);
    buf[n++] = char('0' + (milli / 10) % 10);
    buf[n++] = char('0' + milli % 10);
    buf[n++] = ']';
    return n;
}

std::string elapsedPrefix(int64_t ms) {
    char b[32];
    return std::string(b, formatElapsed(b, ms));
}

#if defined(__APPLE__)
std::string sysctlString(const char* name) {
    size_t len = 0;
    if (sysctlbyname(name, nullptr, &len, nullptr, 0) != 0 || len == 0) return {};
    std::string s(len, '\0');
    if (sysctlbyname(name, s.data(), &len, nullptr, 0) != 0) return {};
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}
uint64_t sysctlU64(const char* name) {
    uint64_t v = 0;
    size_t len = sizeof v;
    if (sysctlbyname(name, &v, &len, nullptr, 0) != 0) return 0;
    if (len == sizeof(uint32_t)) {
        uint32_t v32 = 0;
        std::memcpy(&v32, &v, sizeof v32);
        return v32;
    }
    return v;
}
#endif

// ---- The trace ring ----
struct Ring {
    char* buf = nullptr;
    size_t size = 0;
    std::atomic<uint64_t> head{0}; // bytes ever written
    std::mutex mu;
};
Ring& ring() {
    static Ring* r = new Ring; // never freed: the fatal handler may read it at any time
    return *r;
}
constexpr size_t kRingBytes = 8u << 20;
const int64_t gProcessStartMs = monoMs();
const std::time_t gProcessStartTime = std::time(nullptr);

void ringAppend(std::string_view s) {
    Ring& r = ring();
    std::lock_guard<std::mutex> lock(r.mu);
    if (!r.buf) {
        r.buf = static_cast<char*>(std::malloc(kRingBytes));
        if (!r.buf) return;
        r.size = kRingBytes;
    }
    uint64_t h = r.head.load(std::memory_order_relaxed);
    for (size_t off = 0; off < s.size();) {
        const size_t pos = size_t(h % r.size);
        const size_t n = std::min(s.size() - off, r.size - pos);
        std::memcpy(r.buf + pos, s.data() + off, n);
        off += n;
        h += n;
    }
    r.head.store(h, std::memory_order_release);
}

// ---- The capture ----
struct Capture {
    int logFd = -1;
    int origFd[2] = {-1, -1};
    int readFd[2] = {-1, -1};
    int wake[2] = {-1, -1};
    bool tee[2] = {true, true};
    bool open[2] = {true, true};
    std::string partial[2];
    std::mutex mu;
    std::thread reader;
    pthread_t readerTid{};
    std::atomic<bool> readerTidSet{false};
    std::atomic<bool> busy{false};
    Redactor redactor;
    int64_t startMs = 0;
    uint64_t written = 0, fileCap = 20ull << 20, hardCap = 24ull << 20;
    bool capped = false;
    stdfs::path dir, file;
    std::string stem, prefix;
    char crashTracePath[1024] = {0};
    char logPathText[1024] = {0};
};
std::atomic<Capture*> gCapture{nullptr};
std::atomic<bool> gStopped{false};
std::atomic<bool> gEvents{false};
std::atomic<bool> gTracesOn{false};
std::atomic<bool> gFatalExitForTest{false};

bool keepAfterCap(const std::string& line, bool fromStderr) {
    if (fromStderr) return true;
    if (line.rfind("[ev] life ", 0) == 0 || line.rfind("[ev] net ", 0) == 0) return true;
    std::string l = line;
    for (char& c : l) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (const char* w : {"error", "warning", "fatal", "failed"})
        if (l.find(w) != std::string::npos) return true;
    return false;
}

// One finished line into the file (lock held).
void writeLineLocked(Capture& c, const std::string& raw, bool fromStderr, bool withPrefix = true) {
    if (c.logFd < 0) return;
    std::string text = c.redactor.apply(raw);
    if (c.capped && (c.written >= c.hardCap || !keepAfterCap(text, fromStderr))) return;
    std::string out;
    out.reserve(text.size() + 20);
    if (withPrefix) {
        out = elapsedPrefix(monoMs() - c.startMs);
        out += fromStderr ? "! " : " ";
    }
    out += text;
    out += '\n';
    if (!c.capped && c.written + out.size() > c.fileCap) {
        c.capped = true;
        const std::string note = elapsedPrefix(monoMs() - c.startMs) + " log capped at " + std::to_string(c.fileCap >> 20) +
                                 " MB: from here only errors, warnings and lifecycle lines\n";
        writeAll(c.logFd, note.data(), note.size());
        c.written += note.size();
        if (!keepAfterCap(text, fromStderr)) return;
    }
    writeAll(c.logFd, out.data(), out.size());
    c.written += out.size();
}

constexpr size_t kMaxLine = 16u << 10;

void takeLinesLocked(Capture& c, int i, bool flushPartial) {
    std::string& p = c.partial[i];
    size_t start = 0;
    for (;;) {
        const size_t nl = p.find('\n', start);
        if (nl == std::string::npos) break;
        std::string line = p.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        for (size_t k = 0; k < line.size(); k += kMaxLine) writeLineLocked(c, line.substr(k, kMaxLine), i == 1);
        if (line.empty()) writeLineLocked(c, line, i == 1);
        start = nl + 1;
    }
    p.erase(0, start);
    while (p.size() > kMaxLine) {
        writeLineLocked(c, p.substr(0, kMaxLine), i == 1);
        p.erase(0, kMaxLine);
    }
    if (flushPartial && !p.empty()) {
        writeLineLocked(c, p, i == 1);
        p.clear();
    }
}

// Reads what the pipes hold now (non-blocking), passes it through and writes the finished lines (lock held).
void pumpLocked(Capture& c) {
    char buf[8192];
    for (int i : {1, 0}) { // standard error first: of two lines written about together, the error is the earlier one
        if (!c.open[i]) continue;
        for (;;) {
            const ssize_t n = ::read(c.readFd[i], buf, sizeof buf);
            if (n > 0) {
                if (c.tee[i]) writeAll(c.origFd[i], buf, size_t(n));
                c.partial[i].append(buf, size_t(n));
                continue;
            }
            if (n == 0) c.open[i] = false;           // every write end closed
            else if (errno == EINTR) continue;
            break;                                   // EAGAIN: empty for now
        }
        takeLinesLocked(c, i, !c.open[i]);
    }
}

void readerLoop(Capture* c) {
    c->readerTid = pthread_self();
    c->readerTidSet = true;
    for (;;) {
        pollfd p[3] = {{c->readFd[0], POLLIN, 0}, {c->readFd[1], POLLIN, 0}, {c->wake[0], POLLIN, 0}};
        if (!c->open[0]) p[0].fd = -1;
        if (!c->open[1]) p[1].fd = -1;
        const int r = ::poll(p, 3, -1);
        if (r < 0 && errno == EINTR) continue;
        const bool stop = r < 0 || (p[2].revents != 0);
        c->busy = true;
        {
            std::lock_guard<std::mutex> lock(c->mu);
            pumpLocked(*c);
            if (stop) {
                takeLinesLocked(*c, 0, true);
                takeLinesLocked(*c, 1, true);
            }
        }
        c->busy = false;
        if (stop || (!c->open[0] && !c->open[1])) return;
    }
}

const char* signalName(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGABRT: return "SIGABRT";
    case SIGFPE: return "SIGFPE";
    case SIGTRAP: return "SIGTRAP";
    default: return "signal";
    }
}

// Writes the ring from its oldest byte to fd (no locks, no allocation).
void writeRingRaw(int fd) {
    Ring& r = ring();
    if (!r.buf) return;
    const uint64_t h = r.head.load(std::memory_order_acquire);
    if (h <= r.size) {
        writeAll(fd, r.buf, size_t(h));
        return;
    }
    const size_t pos = size_t(h % r.size);
    writeAll(fd, r.buf + pos, r.size - pos);
    writeAll(fd, r.buf, pos);
}

constexpr int kFatalSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGFPE, SIGTRAP};

void fatalHandler(int sig, siginfo_t*, void*) {
    static volatile sig_atomic_t entered = 0;
    if (entered) {
        ::raise(sig);
        return;
    }
    entered = 1;
    Capture* c = gStopped ? nullptr : gCapture.load();
    if (c && c->logFd >= 0) {
        const bool onReader = c->readerTidSet && pthread_equal(pthread_self(), c->readerTid);
        if (!onReader) // let the reader write what is in the pipes, with the redaction (up to 300 ms)
            for (int k = 0; k < 30; ++k) {
                int n0 = 0, n1 = 0;
                if (c->open[0]) ::ioctl(c->readFd[0], FIONREAD, &n0);
                if (c->open[1]) ::ioctl(c->readFd[1], FIONREAD, &n1);
                if (n0 == 0 && n1 == 0 && !c->busy) break;
                timespec ts{0, 10 * 1000 * 1000};
                ::nanosleep(&ts, nullptr);
            }
        char buf[512];
        size_t n = formatElapsed(buf, monoMs() - c->startMs);
        const char* parts[] = {"! fatal: signal ", nullptr, " (", signalName(sig),
                               "): the log ends here; the crash report is in ~/Library/Logs/DiagnosticReports\n"};
        char num[12];
        size_t k = 0;
        int s = sig;
        do { num[k++] = char('0' + s % 10); s /= 10; } while (s > 0 && k < 10);
        char numStr[12];
        for (size_t j = 0; j < k; ++j) numStr[j] = num[k - 1 - j];
        numStr[k] = '\0';
        parts[1] = numStr;
        for (const char* p : parts)
            for (const char* q = p; *q && n < sizeof buf - 1; ++q) buf[n++] = *q;
        writeAll(c->logFd, buf, n);
#ifdef IW2_HAVE_BACKTRACE
        void* frames[64];
        const int nf = ::backtrace(frames, 64);
        ::backtrace_symbols_fd(frames, nf, c->logFd);
#endif
        char tmp[4096];
        for (int i = 0; i < 2; ++i) // what the reader did not get to (it is this thread, or stuck)
            while (c->open[i]) {
                const ssize_t r = ::read(c->readFd[i], tmp, sizeof tmp);
                if (r <= 0) break;
                writeAll(c->logFd, tmp, size_t(r));
                if (c->tee[i]) writeAll(c->origFd[i], tmp, size_t(r));
            }
        if (c->crashTracePath[0] && ring().buf && ring().head.load() > 0) {
            const int fd = ::open(c->crashTracePath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd >= 0) {
                writeStr(fd, "trace dump (fatal signal), the whole ring\n");
                writeRingRaw(fd);
                ::close(fd);
            }
        }
        if (c->tee[1]) {
            writeStr(c->origFd[1], "fatal: signal; log: ");
            writeStr(c->origFd[1], c->logPathText);
            writeStr(c->origFd[1], "\n");
        }
        ::dup2(c->origFd[0], 1);
        ::dup2(c->origFd[1], 2);
    }
    if (gFatalExitForTest) ::_exit(128 + sig);
    ::raise(sig); // SA_RESETHAND: the default action, the system's crash report
}

void onTerminate() {
    const char* what = "unknown";
    std::string text;
    if (const std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            text = ex.what();
            what = text.c_str();
        } catch (...) {
            what = "not a std::exception";
        }
    } else {
        what = "std::terminate without an exception";
    }
    std::fprintf(stderr, "fatal: uncaught exception: %s\n", what);
    std::fflush(stderr);
    std::abort();
}

void installFatalHandlers() {
    static bool done = false;
    if (done) return;
    done = true;
#ifdef IW2_HAVE_BACKTRACE
    void* warm[4];
    (void)::backtrace(warm, 4); // loads the unwinder now, not in the handler
#endif
    static char* altStack = static_cast<char*>(std::malloc(1u << 16));
    if (altStack) {
        stack_t ss{};
        ss.ss_sp = altStack;
        ss.ss_size = 1u << 16;
        ss.ss_flags = 0;
        ::sigaltstack(&ss, nullptr);
    }
    for (int sig : kFatalSignals) {
        struct sigaction sa {};
        sa.sa_sigaction = fatalHandler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_NODEFER | SA_ONSTACK;
        ::sigaction(sig, &sa, nullptr);
    }
    std::set_terminate(onTerminate);
}

bool isDevNull(int fd) {
    struct stat a {}, b {};
    if (::fstat(fd, &a) != 0 || ::stat("/dev/null", &b) != 0) return false;
    return S_ISCHR(a.st_mode) && a.st_rdev == b.st_rdev;
}

std::string localTimeText(std::time_t t, const char* fmt) {
    std::tm tm{};
    localtime_r(&t, &tm);
    char b[64];
    std::strftime(b, sizeof b, fmt, &tm);
    return b;
}

std::string systemLine() {
    std::string s;
#if defined(__APPLE__)
    s = "macOS " + sysctlString("kern.osproductversion");
    if (const std::string b = sysctlString("kern.osversion"); !b.empty()) s += " (" + b + ")";
    if (const std::string m = sysctlString("hw.model"); !m.empty()) s += ", " + m;
    if (const std::string cpu = sysctlString("machdep.cpu.brand_string"); !cpu.empty()) s += ", " + cpu;
    if (const uint64_t n = sysctlU64("hw.ncpu")) s += ", " + std::to_string(n) + " cores";
    if (const uint64_t mem = sysctlU64("hw.memsize")) s += ", " + std::to_string(mem >> 30) + " GB";
#else
    utsname u{};
    if (::uname(&u) == 0) s = std::string(u.sysname) + " " + u.release + " " + u.machine;
    std::ifstream cpu("/proc/cpuinfo");
    for (std::string l; std::getline(cpu, l);)
        if (l.rfind("model name", 0) == 0) {
            const size_t c = l.find(':');
            if (c != std::string::npos) s += "," + l.substr(c + 1);
            break;
        }
    s += ", " + std::to_string(std::thread::hardware_concurrency()) + " cores";
#endif
    return s;
}

std::vector<std::string> iw2Flags() {
    std::vector<std::string> out;
    for (char** e = environ; e && *e; ++e)
        if (std::strncmp(*e, "IW2_", 4) == 0) out.emplace_back(*e);
    std::sort(out.begin(), out.end());
    return out;
}

std::string crashDirDefault() {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/Library/Logs/DiagnosticReports" : std::string();
}

// The crash reports newer than the newest earlier session of this kind (names only, newest first, up to 3).
std::vector<std::string> crashReportsSince(const stdfs::path& logDir, const std::string& prefix, const std::string& crashDir,
                                           const std::vector<std::string>& crashPrefixes) {
    std::error_code ec;
    stdfs::file_time_type newest{};
    bool any = false;
    for (const auto& e : stdfs::directory_iterator(logDir, ec)) {
        const auto n = parseLogName(e.path().filename().string());
        if (!n || n->prefix != prefix || n->trace) continue;
        const auto t = e.last_write_time(ec);
        if (!ec && (!any || t > newest)) newest = t, any = true;
    }
    std::vector<std::pair<stdfs::file_time_type, std::string>> found;
    if (!any || crashDir.empty()) return {};
    for (const auto& e : stdfs::directory_iterator(crashDir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".ips") != 0) continue;
        bool match = false;
        for (const auto& p : crashPrefixes) match = match || name.rfind(p, 0) == 0;
        if (!match) continue;
        const auto t = e.last_write_time(ec);
        if (!ec && t > newest) found.emplace_back(t, name);
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::string> out;
    for (size_t i = 0; i < found.size() && i < 3; ++i) out.push_back(found[i].second);
    return out;
}

struct Budget {
    int64_t windowStart = 0;
    int count = 0, suppressed = 0;
};
std::mutex gBudgetMu;
std::map<std::string, Budget, std::less<>> gBudgets;
constexpr int kEventBudget = 40;
std::atomic<int64_t> kEventWindowMs{10000};

void emitEventLine(const std::string& line) {
    Capture* c = gCapture.load();
    if (c && !gStopped) {
        std::fflush(stdout);
        std::lock_guard<std::mutex> lock(c->mu);
        pumpLocked(*c); // what this thread printed before comes first
        writeLineLocked(*c, line, false);
        return;
    }
    std::fputs((line + "\n").c_str(), stdout);
}

std::mutex gDumpMu;
int64_t gLastDumpMs = -1000000;
int gDumps = 0;

} // namespace

// ---- Build and platform facts ----
std::string buildVersion() {
#ifdef IW2_VERSION
    return IW2_VERSION;
#else
    return {};
#endif
}
std::string buildType() {
#ifdef IW2_BUILD_TYPE
    return IW2_BUILD_TYPE;
#else
    return {};
#endif
}

std::string executablePath() {
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string p(size, '\0');
    if (_NSGetExecutablePath(p.data(), &size) != 0) return {};
    p.resize(std::strlen(p.c_str()));
    if (char* real = ::realpath(p.c_str(), nullptr)) { // a relative start ("./iw2server") as an absolute path
        p = real;
        std::free(real);
    }
    return p;
#else
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, size_t(n)) : std::string();
#endif
}

bool inAppBundle(std::string_view path) { return path.find(".app/Contents/") != std::string_view::npos; }

bool preRelease(std::string_view v) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.remove_prefix(1);
    return v.size() >= 2 && v[0] == '0' && v[1] == '.';
}

bool loggingWanted(std::string_view version, bool inBundle, const char* env, const char* cvar) {
    for (const char* v : {env, cvar})
        if (v && *v) return std::strcmp(v, "0") != 0;
    return inBundle && preRelease(version);
}

bool eventsWanted(bool logging, const char* env, const char* cvar) {
    for (const char* v : {env, cvar})
        if (v && *v) return std::strcmp(v, "0") != 0;
    return logging;
}

std::optional<std::string> cvarArgument(int argc, char** argv, std::string_view name) {
    std::optional<std::string> out;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--cvar") != 0) continue;
        const std::string_view kv = argv[i + 1];
        const size_t eq = kv.find('=');
        const std::string_view k = kv.substr(0, eq);
        if (k.size() == name.size() && iequalsPrefix(k, name)) out = eq == std::string_view::npos ? std::string("1") : std::string(kv.substr(eq + 1));
    }
    return out;
}

// ---- Folder and names ----
stdfs::path defaultLogDir() {
    if (const char* d = std::getenv("IW2_LOG_DIR"); d && *d) return d;
    const char* home = std::getenv("HOME");
#if defined(__APPLE__)
    return home ? stdfs::path(home) / "Library/Logs/OpenIW2" : stdfs::path();
#else
    if (const char* x = std::getenv("XDG_STATE_HOME"); x && *x) return stdfs::path(x) / "openiw2";
    return home ? stdfs::path(home) / ".local/state/openiw2" : stdfs::path();
#endif
}

std::string sessionStem(std::string_view prefix, std::time_t t, std::string_view suffix) {
    std::string s = std::string(prefix) + "-" + localTimeText(t, "%Y%m%d-%H%M%S");
    if (!suffix.empty()) s += "-" + std::string(suffix);
    return s;
}

std::optional<LogName> parseLogName(std::string_view name) {
    if (name.size() < 5 || name.substr(name.size() - 4) != ".log") return std::nullopt;
    std::string_view body = name.substr(0, name.size() - 4);
    LogName out;
    for (const char* p : {"openiw2-", "iw2server-"})
        if (body.rfind(p, 0) == 0) out.prefix = std::string(p, std::strlen(p) - 1);
    if (out.prefix.empty()) return std::nullopt;
    const size_t tr = body.find("-trace-");
    if (tr != std::string_view::npos) {
        const std::string_view t = body.substr(tr + 7);
        const bool six = t.size() == 6 && std::all_of(t.begin(), t.end(), isDigit);
        if (!six && t != "crash") return std::nullopt;
        out.trace = true;
        body = body.substr(0, tr);
    }
    std::string_view rest = body.substr(out.prefix.size() + 1);
    // YYYYMMDD-HHMMSS, then up to two "-<digits>" groups (a server's port, a same-second number).
    if (rest.size() < 15 || rest[8] != '-') return std::nullopt;
    for (size_t i = 0; i < 15; ++i)
        if (i != 8 && !isDigit(rest[i])) return std::nullopt;
    std::string_view tail = rest.substr(15);
    for (int groups = 0; !tail.empty(); ++groups) {
        if (groups == 2 || tail[0] != '-' || tail.size() < 2) return std::nullopt;
        size_t k = 1;
        while (k < tail.size() && isDigit(tail[k])) ++k;
        if (k == 1 || (k < tail.size() && tail[k] != '-')) return std::nullopt;
        tail = tail.substr(k);
    }
    out.session = std::string(body);
    return out;
}

bool legacyLogName(std::string_view n) {
    if (n == "openiw2.log" || n == "openiw2.log.1") return true;
    return n.size() == 13 && n.rfind("openiw2-", 0) == 0 && n[8] >= '1' && n[8] <= '9' && n.substr(9) == ".log";
}

void pruneLogs(const stdfs::path& dir, const std::string& current, size_t keepPerPrefix, uint64_t capBytes) {
    std::error_code ec;
    struct Session {
        std::string prefix, stamp;
        std::vector<stdfs::path> files;
        uint64_t bytes = 0;
    };
    std::map<std::string, Session> sessions;
    for (const auto& e : stdfs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const std::string name = e.path().filename().string();
        if (legacyLogName(name)) {
            stdfs::remove(e.path(), ec);
            continue;
        }
        const auto n = parseLogName(name);
        if (!n) continue;
        Session& s = sessions[n->session];
        s.prefix = n->prefix;
        s.stamp = n->session.substr(n->prefix.size() + 1);
        s.files.push_back(e.path());
        s.bytes += e.file_size(ec);
    }
    const auto remove = [&](const std::string& stem) {
        for (const auto& f : sessions[stem].files) stdfs::remove(f, ec);
        sessions.erase(stem);
    };
    const auto currentName = parseLogName(current + ".log");
    for (const char* prefix : {"openiw2", "iw2server"}) {
        std::vector<std::string> stems;
        for (const auto& [stem, s] : sessions)
            if (s.prefix == prefix && stem != current) stems.push_back(stem);
        std::sort(stems.begin(), stems.end(), [&](const std::string& a, const std::string& b) { return sessions[a].stamp > sessions[b].stamp; });
        const size_t keep = keepPerPrefix - ((currentName && currentName->prefix == prefix && keepPerPrefix > 0) ? 1 : 0);
        for (size_t i = keep; i < stems.size(); ++i) remove(stems[i]);
    }
    for (;;) {
        uint64_t total = 0;
        std::string oldest;
        for (const auto& [stem, s] : sessions) {
            total += s.bytes;
            if (stem != current && (oldest.empty() || s.stamp < sessions[oldest].stamp)) oldest = stem;
        }
        if (total <= capBytes || oldest.empty()) break;
        remove(oldest);
    }
}

// ---- Redaction ----
namespace {
bool privateOrLoopback(int a, int b) {
    return a == 127 || a == 10 || a == 0 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254);
}

// Masks the first two numbers' successors of public IPv4 addresses.
std::string maskAddresses(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const bool boundary = i == 0 || !(isDigit(s[i - 1]) || s[i - 1] == '.');
        if (boundary && isDigit(s[i])) {
            int parts[4];
            size_t j = i, k = 0;
            bool ok = true;
            for (; k < 4; ++k) {
                size_t d = 0;
                int v = 0;
                while (j < s.size() && isDigit(s[j]) && d < 4) v = v * 10 + (s[j++] - '0'), ++d;
                if (d == 0 || d > 3 || v > 255) { ok = false; break; }
                parts[k] = v;
                if (k < 3) {
                    if (j >= s.size() || s[j] != '.') { ok = false; break; }
                    ++j;
                }
            }
            if (ok && k == 4 && !(j < s.size() && (isDigit(s[j]) || (s[j] == '.' && j + 1 < s.size() && isDigit(s[j + 1]))))) {
                if (privateOrLoopback(parts[0], parts[1])) out.append(s, i, j - i);
                else out += std::to_string(parts[0]) + "." + std::to_string(parts[1]) + ".x.x";
                i = j;
                continue;
            }
            while (i < s.size() && (isDigit(s[i]) || s[i] == '.')) out += s[i++];
            continue;
        }
        out += s[i++];
    }
    return out;
}

// The token at p (quoted: up to the closing quote; bare: up to white space, a quote or a backslash) becomes ***.
size_t maskToken(std::string& s, size_t p) {
    if (p >= s.size()) return p;
    size_t b = p, e;
    if (s[p] == '"') {
        b = p + 1;
        e = s.find('"', b);
        if (e == std::string::npos) e = s.size();
    } else {
        e = p;
        while (e < s.size() && s[e] != ' ' && s[e] != '\t' && s[e] != '"' && s[e] != '\\') ++e;
    }
    if (e == b) return e;
    s.replace(b, e - b, "***");
    return b + 3;
}

void maskRcon(std::string& s) {
    for (size_t p = s.find("rcon "); p != std::string::npos; p = s.find("rcon ", p + 1)) {
        const char prev = p == 0 ? ' ' : s[p - 1];
        if (!(p == 0 || prev == ' ' || prev == '"' || prev == '\'' || prev == ']' || prev == '+' || prev == '\t' || prev == '\xff')) continue;
        size_t t = p + 5;
        while (t < s.size() && s[t] == ' ') ++t;
        if (s.compare(t, 5, "from ") == 0 || s.compare(t, 4, "from") == 0) continue;
        maskToken(s, t);
    }
}

void maskPasswords(std::string& s) {
    size_t from = 0;
    for (;;) {
        size_t hit = std::string::npos;
        for (size_t i = from; i + 8 <= s.size(); ++i)
            if (iequalsPrefix(std::string_view(s).substr(i), "password")) { hit = i; break; }
        if (hit == std::string::npos) return;
        size_t b = hit, e = hit + 8;
        while (b > 0 && isIdent(s[b - 1])) --b;
        while (e < s.size() && isIdent(s[e])) ++e;
        from = e;
        const std::string_view name(s.data() + b, e - b);
        const bool info = b > 0 && s[b - 1] == '\\' && e < s.size() && s[e] == '\\';
        const bool assigned = e < s.size() && s[e] == '='; // password=x (a cvar list)
        const bool column = e + 1 < s.size() && s[e] == ' ' && s[e + 1] == ' ' && b == 0; // dumpuser: "password            x"
        if (name.find('_') == std::string_view::npos && !info && !assigned && !column) continue;
        size_t p = e;
        if (info) {
            maskToken(s, p + 1);
            continue;
        }
        if (p < s.size() && s[p] == '"' && b > 0 && s[b - 1] == '"') ++p; // "name" is:"value"
        if (p >= s.size() || !(s[p] == ' ' || s[p] == '\t' || s[p] == '=' || s[p] == ':')) continue;
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '=' || s[p] == ':')) ++p;
        if (s.compare(p, 3, "is:") == 0) p += 3;
        from = maskToken(s, p);
    }
}
} // namespace

std::string Redactor::apply(std::string_view line) const {
    std::string s(line);
    if (home_.size() > 1)
        for (size_t p = s.find(home_); p != std::string::npos; p = s.find(home_, p + 1)) {
            const size_t e = p + home_.size();
            if (e < s.size() && isIdent(s[e])) continue; // a home of /srv/ab is not /srv/abc
            s.replace(p, home_.size(), "~");
        }
    if (s.rfind("[chat] ", 0) == 0) {
        const size_t c = s.find(": ", 7);
        if (c == std::string::npos) return "[chat] (" + std::to_string(s.size() - 7) + " chars)";
        return s.substr(0, c) + ": (" + std::to_string(s.size() - c - 2) + " chars)";
    }
    // A client command the server does not know, "client <n> command: <text>": the command word stays.
    if (s.rfind("client ", 0) == 0) {
        const size_t p = s.find(" command: ");
        if (p != std::string::npos && p > 7 && std::all_of(s.begin() + 7, s.begin() + long(p), isDigit)) {
            const size_t w = p + 10, sp = s.find(' ', w);
            if (sp != std::string::npos) return s.substr(0, sp) + " (" + std::to_string(s.size() - sp - 1) + " chars)";
            return s;
        }
    }
    if (s.size() >= 3 && (s[0] == 'h' || s[0] == 'i') && s[1] == ' ' && s[2] == '"') {
        size_t n = s.size() - 3;
        if (n > 0 && s.back() == '"') --n;
        return s.substr(0, 3) + "(" + std::to_string(n) + " chars)\"";
    }
    maskRcon(s);
    maskPasswords(s);
    return maskAddresses(s);
}

// ---- Capture ----
bool startCapture(const CaptureOptions& o) {
    if (gCapture.load() && !gStopped) return true; // a stopped capture may be followed by a new one (tests)
    std::error_code ec;
    stdfs::create_directories(o.dir, ec);
    if (o.dir.empty() || !stdfs::is_directory(o.dir, ec)) return false;
    auto* c = new Capture;
    c->prefix = o.prefix;
    c->dir = o.dir;
    c->fileCap = o.fileCap;
    c->hardCap = std::max(o.hardCap, o.fileCap);
    const char* home = std::getenv("HOME");
    c->redactor = Redactor(home ? home : "");
    const std::vector<std::string> crashes =
        crashReportsSince(o.dir, o.prefix, o.crashDir.empty() ? crashDirDefault() : o.crashDir, o.crashPrefixes);
    const std::time_t now = std::time(nullptr);
    const std::string base = sessionStem(o.prefix, now);
    pruneLogs(o.dir, base, 10, 100ull << 20);
    for (int k = 1; k <= 9 && c->logFd < 0; ++k) {
        c->stem = k == 1 ? base : base + "-" + std::to_string(k);
        c->file = o.dir / (c->stem + ".log");
        c->logFd = ::open(c->file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0644);
        if (c->logFd < 0 && errno != EEXIST) break;
    }
    if (c->logFd < 0) {
        delete c;
        return false;
    }
    int p0[2], p1[2];
    if (::pipe(p0) != 0) { ::close(c->logFd); delete c; return false; }
    if (::pipe(p1) != 0) { ::close(p0[0]); ::close(p0[1]); ::close(c->logFd); delete c; return false; }
    if (::pipe(c->wake) != 0) { for (int fd : {p0[0], p0[1], p1[0], p1[1], c->logFd}) ::close(fd); delete c; return false; }
    for (int fd : {p0[0], p1[0], c->wake[0], c->wake[1]}) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    for (int fd : {p0[0], p1[0]}) ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    std::fflush(stdout);
    std::fflush(stderr);
    c->origFd[0] = ::fcntl(1, F_DUPFD_CLOEXEC, 3);
    c->origFd[1] = ::fcntl(2, F_DUPFD_CLOEXEC, 3);
    c->tee[0] = c->origFd[0] >= 0 && !isDevNull(c->origFd[0]);
    c->tee[1] = c->origFd[1] >= 0 && !isDevNull(c->origFd[1]);
    c->readFd[0] = p0[0];
    c->readFd[1] = p1[0];
    c->startMs = monoMs();
    std::snprintf(c->logPathText, sizeof c->logPathText, "%s", c->redactor.apply(c->file.string()).c_str());
    std::snprintf(c->crashTracePath, sizeof c->crashTracePath, "%s", (o.dir / (c->stem + "-trace-crash.log")).c_str());

    // The header, straight into the file.
    {
        std::vector<std::string> h;
        std::string first = "OpenIW2 log, " + o.program + " " + (buildVersion().empty() ? std::string("(unknown version)") : buildVersion());
        if (!buildType().empty()) first += " (" + buildType() + ")";
        first += ", session " + localTimeText(now, "%Y-%m-%d %H:%M:%S %z");
        h.push_back(first);
        const std::string exe = executablePath();
        h.push_back(std::string("binary: ") + (inAppBundle(exe) ? "app bundle, " : "development build, ") + exe);
        h.push_back("system: " + systemLine());
        h.push_back("log: " + c->file.string());
        const auto flags = iw2Flags();
        std::string f;
        for (const auto& x : flags) f += (f.empty() ? "" : " ") + x;
        h.push_back("flags: " + (f.empty() ? std::string("none") : f));
        for (const auto& n : o.traceNotes) h.push_back("traces: " + n);
        for (const auto& cr : crashes) h.push_back("previous session: crash report " + cr);
        for (const auto& line : h) writeLineLocked(*c, line, false, false);
    }

    if (::dup2(p0[1], 1) < 0 || ::dup2(p1[1], 2) < 0) {
        ::dup2(c->origFd[0], 1);
        ::dup2(c->origFd[1], 2);
        for (int fd : {p0[0], p0[1], p1[0], p1[1], c->wake[0], c->wake[1], c->logFd, c->origFd[0], c->origFd[1]}) ::close(fd);
        delete c;
        return false;
    }
    ::close(p0[1]);
    ::close(p1[1]);
    // A pipe would make stdout fully buffered: line-buffered, so the log is current and a crash loses no finished line
    // (setvbuf flushes first on the platforms built for; stderr stays unbuffered).
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    gEvents = o.events;
    gStopped = false;
    gCapture = c;
    c->reader = std::thread(readerLoop, c);
    static bool atExit = false;
    if (!atExit) {
        atExit = true;
        std::atexit(stopCapture);
    }
    installFatalHandlers();
    return true;
}

bool programLogWanted(int argc, char** argv) {
    const auto cv = cvarArgument(argc, argv, "iw2_log");
    return loggingWanted(buildVersion(), inAppBundle(executablePath()), std::getenv("IW2_LOG"), cv ? cv->c_str() : nullptr);
}

bool startProgramLog(int argc, char** argv, const std::string& prefix, const std::string& program,
                     const std::vector<std::string>& crashPrefixes, const std::string& traceText) {
    const auto ev = cvarArgument(argc, argv, "iw2_logEvents");
    const bool wanted = programLogWanted(argc, argv);
    const bool events = eventsWanted(wanted, std::getenv("IW2_LOG_EVENTS"), ev ? ev->c_str() : nullptr);
    if (!wanted) {
        gEvents = events; // IW2_LOG_EVENTS=1 without a log file: event lines on stdout
        return false;
    }
    CaptureOptions o;
    o.dir = defaultLogDir();
    o.prefix = prefix;
    o.program = program;
    o.events = events;
    o.crashPrefixes = crashPrefixes;
    std::string text = traceText;
    {
        std::ifstream f(o.dir / "traces.txt");
        const std::string t((std::istreambuf_iterator<char>(f)), {});
        if (!t.empty()) text += (text.empty() ? "" : "\n") + t;
    }
    if (!text.empty()) o.traceNotes = applyTraceSwitches(text);
    if (!startCapture(o)) {
        gEvents = false;
        return false;
    }
    return true;
}

bool captureActive() { return gCapture.load() && !gStopped; }
stdfs::path logPath() {
    Capture* c = gCapture.load();
    return c ? c->file : stdfs::path();
}
std::string sessionName() {
    Capture* c = gCapture.load();
    return c ? c->stem : std::string();
}

bool renameSession(const std::string& suffix) {
    Capture* c = gCapture.load();
    if (!c || suffix.empty()) return false;
    std::lock_guard<std::mutex> lock(c->mu);
    const std::string stem = c->stem + "-" + suffix;
    const stdfs::path to = c->dir / (stem + ".log");
    std::error_code ec;
    if (stdfs::exists(to, ec)) return false;
    stdfs::rename(c->file, to, ec);
    if (ec) return false;
    c->file = to;
    c->stem = stem;
    std::snprintf(c->logPathText, sizeof c->logPathText, "%s", c->redactor.apply(c->file.string()).c_str());
    std::snprintf(c->crashTracePath, sizeof c->crashTracePath, "%s", (c->dir / (stem + "-trace-crash.log")).c_str());
    return true;
}

void drain() {
    Capture* c = gCapture.load();
    std::fflush(stdout);
    std::fflush(stderr);
    if (!c || gStopped) return;
    std::lock_guard<std::mutex> lock(c->mu);
    pumpLocked(*c);
}

void stopCapture() {
    Capture* c = gCapture.load();
    if (!c || gStopped.exchange(true)) return;
    if (ring().head.load() > 0) { // traces were on: their last minute goes to a trace file
        gStopped = false; // the exit dump still names the session
        dumpTrace("exit");
        gStopped = true;
    }
    std::fflush(stdout);
    std::fflush(stderr);
    ::dup2(c->origFd[0], 1); // later output goes to the original descriptors
    ::dup2(c->origFd[1], 2);
    const char w = 'q';
    writeAll(c->wake[1], &w, 1);
    if (c->reader.joinable()) c->reader.join();
    std::lock_guard<std::mutex> lock(c->mu);
    pumpLocked(*c);
    takeLinesLocked(*c, 0, true);
    takeLinesLocked(*c, 1, true);
}

void headerLine(std::string_view key, std::string_view value) {
    Capture* c = gCapture.load();
    if (!c || gStopped) return;
    std::fflush(stdout);
    std::lock_guard<std::mutex> lock(c->mu);
    pumpLocked(*c);
    writeLineLocked(*c, std::string(key) + ": " + std::string(value), false);
}

// ---- Event lines ----
bool eventsEnabled() { return gEvents; }
void setEventsEnabled(bool on) { gEvents = on; }

void event(std::string_view category, std::string_view text) {
    if (!gEvents) return;
    const int64_t now = monoMs();
    std::string suppressedNote;
    {
        std::lock_guard<std::mutex> lock(gBudgetMu);
        auto it = gBudgets.find(category);
        if (it == gBudgets.end()) it = gBudgets.emplace(std::string(category), Budget{now, 0, 0}).first;
        Budget& b = it->second;
        if (now - b.windowStart >= kEventWindowMs) {
            if (b.suppressed > 0)
                suppressedNote = "[ev] " + std::string(category) + " " + std::to_string(b.suppressed) + " lines suppressed";
            b = Budget{now, 0, 0};
        }
        if (b.count >= kEventBudget) {
            ++b.suppressed;
            return;
        }
        ++b.count;
    }
    if (!suppressedNote.empty()) emitEventLine(suppressedNote);
    emitEventLine("[ev] " + std::string(category) + " " + std::string(text));
}

// ---- Trace recorder ----
void trace(std::string_view category, std::string_view line, std::FILE* outFile) {
    (void)category;
    std::string s = elapsedPrefix(monoMs() - (gCapture.load() ? gCapture.load()->startMs : gProcessStartMs));
    s += ' ';
    s.append(line);
    if (s.empty() || s.back() != '\n') s += '\n';
    ringAppend(s);
    std::string_view out = line;
    if (!captureActive()) {
        std::fwrite(out.data(), 1, out.size(), outFile);
        if (out.empty() || out.back() != '\n') std::fputc('\n', outFile);
        return;
    }
    // With the log file on: not into the file, but still to the terminal, file or pipe the program was started with.
    Capture* c = gCapture.load();
    const int i = outFile == stderr ? 1 : 0;
    if (c && c->tee[i]) {
        std::string t(out);
        if (t.empty() || t.back() != '\n') t += '\n';
        writeAll(c->origFd[i], t.data(), t.size());
    }
}

void tracef(const char* category, const char* format, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, format);
    const int n = std::vsnprintf(buf, sizeof buf, format, ap);
    va_end(ap);
    if (n < 0) return;
    if (size_t(n) < sizeof buf) {
        trace(category, std::string_view(buf, size_t(n)));
        return;
    }
    std::string big(size_t(n) + 1, '\0');
    va_start(ap, format);
    std::vsnprintf(big.data(), big.size(), format, ap);
    va_end(ap);
    big.pop_back();
    trace(category, big);
}

Trace::~Trace() { trace(category_, text_.str(), out_); }

std::string dumpTrace(std::string_view reason) {
    const bool always = reason == "command";
    // Without a log file the trace lines went to stdout already: only dumptrace writes a file then (a development run
    // never fills the real log folder by itself).
    if (!always && !captureActive()) return {};
    const int64_t now = monoMs();
    std::lock_guard<std::mutex> dl(gDumpMu);
    if (!always && (now - gLastDumpMs < 10000 || gDumps >= 20)) return {};
    std::string text;
    {
        Ring& r = ring();
        std::lock_guard<std::mutex> lock(r.mu);
        if (!r.buf || r.head.load() == 0) return {};
        const uint64_t h = r.head.load();
        if (h <= r.size) text.assign(r.buf, size_t(h));
        else {
            const size_t pos = size_t(h % r.size);
            text.assign(r.buf + pos, r.size - pos);
            text.append(r.buf, pos);
            const size_t nl = text.find('\n');
            text.erase(0, nl == std::string::npos ? text.size() : nl + 1); // a cut first line
        }
    }
    Capture* c = captureActive() ? gCapture.load() : nullptr; // a stopped capture's folder is not used
    const int64_t elapsedNow = now - (gCapture.load() ? gCapture.load()->startMs : gProcessStartMs);
    // Only the last 60 s: lines start with [m:ss.mmm].
    const auto stampOf = [](std::string_view l) -> int64_t {
        if (l.size() < 10 || l[0] != '[') return -1;
        const size_t colon = l.find(':'), close = l.find(']');
        if (colon == std::string_view::npos || close == std::string_view::npos || close < colon) return -1;
        int64_t m = 0;
        for (size_t i = 1; i < colon; ++i) m = m * 10 + (l[i] - '0');
        const int64_t sec = (l[colon + 1] - '0') * 10 + (l[colon + 2] - '0');
        const int64_t ms = (l[colon + 4] - '0') * 100 + (l[colon + 5] - '0') * 10 + (l[colon + 6] - '0');
        return m * 60000 + sec * 1000 + ms;
    };
    size_t from = 0;
    while (from < text.size()) {
        const size_t nl = text.find('\n', from);
        const int64_t t = stampOf(std::string_view(text).substr(from, nl == std::string::npos ? std::string::npos : nl - from));
        if (t >= elapsedNow - 60000) break;
        if (nl == std::string::npos) { from = text.size(); break; }
        from = nl + 1;
    }
    text.erase(0, from);
    if (text.empty()) return {};
    const stdfs::path dir = c ? c->dir : defaultLogDir();
    if (dir.empty()) return {};
    std::error_code ec;
    stdfs::create_directories(dir, ec);
    const std::string stem = c ? c->stem : sessionStem(stdfs::path(executablePath()).filename() == "iw2server" ? "iw2server" : "openiw2", gProcessStartTime);
    std::string name = stem + "-trace-" + localTimeText(std::time(nullptr), "%H%M%S");
    stdfs::path file = dir / (name + ".log");
    for (int k = 2; stdfs::exists(file, ec) && k < 10; ++k) file = dir / (name + "-" + std::to_string(k) + ".log");
    if (stdfs::exists(file, ec)) return {};
    std::ofstream out(file, std::ios::binary);
    if (!out) return {};
    const Redactor red(std::getenv("HOME") ? std::getenv("HOME") : "");
    out << "trace dump (" << reason << ") of " << stem << ", the last 60 s of the trace recorder\n";
    for (size_t p = 0; p < text.size();) {
        const size_t nl = text.find('\n', p);
        const std::string_view l = std::string_view(text).substr(p, nl == std::string::npos ? std::string::npos : nl - p);
        out << red.apply(l) << "\n";
        if (nl == std::string::npos) break;
        p = nl + 1;
    }
    gLastDumpMs = now;
    ++gDumps;
    std::printf("trace dump (%s): %s\n", std::string(reason).c_str(), file.string().c_str());
    std::fflush(stdout);
    return file.string();
}

namespace {
// The trace switches docs/logging.md accepts from trace= / traces.txt.
constexpr const char* kTraceSwitches[] = {
    "IW2_FRAME_TRACE", "IW2_PRED_TRACE", "IW2_NET_TRACE", "IW2_KILLCAM_TRACE", "IW2_SOUND_LOG", "IW2_BOB_TRACE",
    "IW2_STANCE_TRACE", "IW2_DAMAGE_TRACE", "IW2_HINT_LOG", "IW2_START_TRACE", "IW2_VM_TRACE", "IW2_RIDE_TRACE",
    "IW2_REMOTE_TRACE", "IW2_WEAPON_TRACE", "IW2_SOAK_SHOTTRACE",
};
} // namespace

std::vector<std::string> applyTraceSwitches(std::string_view text, int* applied) {
    std::vector<std::string> notes;
    int n = 0;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == ',' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r' || text[i] == ';')) ++i;
        size_t e = i;
        while (e < text.size() && !(text[e] == ' ' || text[e] == ',' || text[e] == '\t' || text[e] == '\n' || text[e] == '\r' || text[e] == ';')) ++e;
        if (e == i) break;
        const std::string tok(text.substr(i, e - i));
        i = e;
        if (tok[0] == '#') { // a comment to the end of the line
            while (i < text.size() && text[i] != '\n') ++i;
            continue;
        }
        const size_t eq = tok.find('=');
        const std::string name = tok.substr(0, eq), value = eq == std::string::npos ? "1" : tok.substr(eq + 1);
        const bool known = std::any_of(std::begin(kTraceSwitches), std::end(kTraceSwitches), [&](const char* k) { return name == k; });
        if (!known || value.empty()) {
            notes.push_back("ignored " + tok + " (not a trace switch)");
            continue;
        }
        if (const char* have = std::getenv(name.c_str())) {
            notes.push_back(name + "=" + have + " (from the environment)");
        } else {
            ::setenv(name.c_str(), value.c_str(), 0);
            notes.push_back(name + "=" + value);
        }
        ++n;
    }
    if (n > 0) gTracesOn = true;
    if (applied) *applied = n;
    return notes;
}

std::string traceLineOf(const stdfs::path& settingsFile) {
    std::ifstream f(settingsFile);
    std::string line, out;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("trace=", 0) == 0) out = line.substr(6);
    }
    return out;
}

bool tracesSwitchedOn() { return gTracesOn; }

void resetRingForTest(size_t bytes) {
    Ring& r = ring();
    std::lock_guard<std::mutex> lock(r.mu);
    std::free(r.buf);
    r.buf = static_cast<char*>(std::malloc(bytes));
    r.size = r.buf ? bytes : 0;
    r.head = 0;
    std::lock_guard<std::mutex> dl(gDumpMu);
    gLastDumpMs = -1000000;
    gDumps = 0;
}

void setFatalExitForTest(bool on) { gFatalExitForTest = on; }
void setEventWindowForTest(int64_t ms) { kEventWindowMs = ms; }

std::string ringContents() {
    Ring& r = ring();
    std::lock_guard<std::mutex> lock(r.mu);
    if (!r.buf) return {};
    const uint64_t h = r.head.load();
    if (h <= r.size) return std::string(r.buf, size_t(h));
    const size_t pos = size_t(h % r.size);
    return std::string(r.buf + pos, r.size - pos) + std::string(r.buf, pos);
}

} // namespace iw2::log
