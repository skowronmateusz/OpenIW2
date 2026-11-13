// Session logs (docs/logging.md, src/util/log.h): gating, names, pruning, redaction, the fd capture with its
// pass-through, header, event budget, trace recorder and dumps, trace switches, and the fatal-signal path (in a
// child process). Everything is written below a temp folder; nothing goes to the real log folder.
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "util/log.h"

extern char** environ;

namespace stdfs = std::filesystem;
using namespace iw2::log;

static int failures = 0;
static int reportFd = 2; // a copy of the original stderr: CHECK messages never go into a capture
#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            dprintf(reportFd, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);         \
            ++failures;                                                                        \
        }                                                                                      \
    } while (0)

static std::string readFile(const stdfs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), {});
}
static void writeFile(const stdfs::path& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; }
static bool contains(const std::string& s, const std::string& x) { return s.find(x) != std::string::npos; }
static int countOf(const std::string& s, const std::string& x) {
    int n = 0;
    for (size_t p = s.find(x); p != std::string::npos; p = s.find(x, p + 1)) ++n;
    return n;
}
static std::vector<std::string> filesIn(const stdfs::path& d) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : stdfs::directory_iterator(d, ec)) out.push_back(e.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

static void gating() {
    CHECK(preRelease("v0.3.0") && preRelease("0.2.5-3-g1a2b3c4-dirty") && preRelease("v0.10.1"));
    CHECK(!preRelease("v1.0.0") && !preRelease("1a2b3c4") && !preRelease("") && !preRelease("v10.1") && !preRelease("0"));
    // the default: a 0.x version in an app bundle
    CHECK(loggingWanted("v0.3.0", true, nullptr, nullptr));
    CHECK(!loggingWanted("v0.3.0", false, nullptr, nullptr));
    CHECK(!loggingWanted("v1.0.0", true, nullptr, nullptr));
    CHECK(!loggingWanted("", true, nullptr, nullptr));
    // overrides: the environment first, then the cvar; "0" is off
    CHECK(!loggingWanted("v0.3.0", true, "0", "1"));
    CHECK(loggingWanted("v1.0.0", false, "1", "0"));
    CHECK(loggingWanted("v1.0.0", false, nullptr, "1") && !loggingWanted("v0.3.0", true, nullptr, "0"));
    CHECK(loggingWanted("v0.3.0", true, "", nullptr)); // an empty value counts as unset
    CHECK(eventsWanted(true, nullptr, nullptr) && !eventsWanted(false, nullptr, nullptr));
    CHECK(!eventsWanted(true, "0", nullptr) && !eventsWanted(true, nullptr, "0") && eventsWanted(false, "1", nullptr));
    CHECK(inAppBundle("/Applications/OpenIW2.app/Contents/MacOS/iw2server") && !inAppBundle("/src/build/iw2server"));
    {
        const char* a[] = {"iw2view", "/main", "--cvar", "iw2_log=0", "--cvar", "IW2_LOGEVENTS=1", "--cvar", "x=2", "--cvar", "iw2_log=1"};
        char** argv = const_cast<char**>(a);
        CHECK(cvarArgument(10, argv, "iw2_log") == std::optional<std::string>("1")); // the last one
        CHECK(cvarArgument(10, argv, "iw2_logEvents") == std::optional<std::string>("1")); // case-insensitive name
        CHECK(!cvarArgument(10, argv, "iw2"));
        CHECK(!cvarArgument(4, argv, "x"));
    }
}

static void names(const stdfs::path& root) {
    std::tm tm{};
    tm.tm_year = 2026 - 1900, tm.tm_mon = 10, tm.tm_mday = 13, tm.tm_hour = 16, tm.tm_min = 8, tm.tm_sec = 5, tm.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm);
    CHECK(sessionStem("openiw2", t) == "openiw2-20261113-160805");
    CHECK(sessionStem("iw2server", t, "28960") == "iw2server-20261113-160805-28960");
    const auto a = parseLogName("openiw2-20261113-160805.log");
    CHECK(a && a->prefix == "openiw2" && a->session == "openiw2-20261113-160805" && !a->trace);
    const auto b = parseLogName("iw2server-20261113-160805-28960-2-trace-161530.log");
    CHECK(b && b->prefix == "iw2server" && b->session == "iw2server-20261113-160805-28960-2" && b->trace);
    const auto c = parseLogName("openiw2-20261113-160805-trace-crash.log");
    CHECK(c && c->trace && c->session == "openiw2-20261113-160805");
    for (const char* bad : {"openiw2.log", "openiw2-1.log", "openiw2-2026111-160805.log", "openiw2-20261113-160805.txt", "other-20261113-160805.log",
                            "openiw2-20261113-160805-x.log", "openiw2-20261113-160805-1-2-3.log", "openiw2-20261113-160805-trace-1.log", "traces.txt"})
        CHECK(!parseLogName(bad));
    CHECK(legacyLogName("openiw2.log") && legacyLogName("openiw2-4.log") && legacyLogName("openiw2.log.1"));
    CHECK(!legacyLogName("openiw2-10.log") && !legacyLogName("openiw2-20261113-160805.log"));

    // Pruning: 12 client sessions (two with traces), 3 server sessions, legacy names and foreign files.
    const stdfs::path d = root / "prune";
    stdfs::create_directories(d);
    for (int i = 0; i < 12; ++i) {
        char n[64];
        std::snprintf(n, sizeof n, "openiw2-202611%02d-120000", i + 1);
        writeFile(d / (std::string(n) + ".log"), "x");
        if (i == 0 || i == 11) writeFile(d / (std::string(n) + "-trace-120500.log"), "t");
    }
    for (int i = 0; i < 3; ++i) writeFile(d / ("iw2server-2026110" + std::to_string(i + 1) + "-120000-28960.log"), "s");
    for (const char* f : {"openiw2.log", "openiw2-1.log", "openiw2-4.log", "openiw2.log.1"}) writeFile(d / f, "legacy");
    for (const char* f : {"traces.txt", "notes.log", "openiw2-10.log", "readme"}) writeFile(d / f, "keep");
    const std::string current = "openiw2-20261201-090000";
    pruneLogs(d, current, 10, 100ull << 20);
    const auto left = filesIn(d);
    int clients = 0, servers = 0, traces = 0;
    for (const auto& f : left)
        if (auto n = parseLogName(f)) (n->trace ? traces : n->prefix == "openiw2" ? clients : servers)++;
    CHECK(clients == 9); // with the coming session 10
    CHECK(servers == 3 && traces == 1);
    CHECK(!stdfs::exists(d / "openiw2-20261101-120000.log") && !stdfs::exists(d / "openiw2-20261101-120000-trace-120500.log")); // the oldest, with its trace
    CHECK(stdfs::exists(d / "openiw2-20261112-120000-trace-120500.log"));
    for (const char* f : {"openiw2.log", "openiw2-1.log", "openiw2-4.log", "openiw2.log.1"}) CHECK(!stdfs::exists(d / f));
    for (const char* f : {"traces.txt", "notes.log", "openiw2-10.log", "readme"}) CHECK(stdfs::exists(d / f));
    // The folder cap: the oldest sessions of either kind go first, never the current one.
    writeFile(d / (current + ".log"), std::string(3000, 'c'));
    writeFile(d / "openiw2-20261112-120000.log", std::string(1000, 'n'));
    pruneLogs(d, current, 10, 4011); // 4012 bytes: the oldest session (of either kind) goes
    CHECK(stdfs::exists(d / (current + ".log")) && stdfs::exists(d / "openiw2-20261112-120000.log"));
    CHECK(!stdfs::exists(d / "iw2server-20261101-120000-28960.log") && stdfs::exists(d / "iw2server-20261102-120000-28960.log"));
    CHECK(stdfs::exists(d / "openiw2-20261104-120000.log"));
    pruneLogs(d, current, 10, 10);
    CHECK(stdfs::exists(d / (current + ".log")) && !stdfs::exists(d / "openiw2-20261112-120000.log"));
    CHECK(stdfs::exists(d / "traces.txt") && stdfs::exists(d / "notes.log"));
}

static void redaction() {
    const Redactor r("/srv/tester");
    CHECK(r.apply("game folder: /srv/tester/Documents/main") == "game folder: ~/Documents/main");
    CHECK(r.apply("/srv/testerx/a and /srv/tester") == "/srv/testerx/a and ~");
    // rcon: the request line after "Rcon from", also with a wrong password; "from" itself stays
    CHECK(r.apply("Bad rcon from 192.168.1.5:28961:") == "Bad rcon from 192.168.1.5:28961:");
    CHECK(r.apply("rcon s3cret status") == "rcon *** status");
    CHECK(r.apply("rcon \"two words\" map mp_toujane") == "rcon \"***\" map mp_toujane");
    CHECK(r.apply("]rcon pw kick bob") == "]rcon *** kick bob");
    // password cvars in their print forms
    CHECK(r.apply("set rcon_password pw") == "set rcon_password ***");
    CHECK(r.apply("+set g_password \"secret word\" +map x") == "+set g_password \"***\" +map x");
    CHECK(r.apply("\"rcon_password\" is:\"pw^7\" default:\"^7\"") == "\"rcon_password\" is:\"***\" default:\"^7\"");
    CHECK(r.apply("userinfo \\name\\bob\\password\\hunter2\\rate\\25000") == "userinfo \\name\\bob\\password\\***\\rate\\25000");
    CHECK(r.apply("sv_privatePassword=abc next") == "sv_privatePassword=*** next");
    CHECK(r.apply("IW2_X=1 g_password: x1") == "IW2_X=1 g_password: ***");
    CHECK(r.apply("Invalid password.") == "Invalid password.");
    CHECK(r.apply("The server must set 'rcon_password' for clients to use 'rcon'.") == "The server must set 'rcon_password' for clients to use 'rcon'.");
    CHECK(r.apply("set rcon_password \"\"") == "set rcon_password \"\"");
    // chat: the speaker stays, the text is counted
    CHECK(r.apply("[chat] (DEAD)Bob: hello there") == "[chat] (DEAD)Bob: (11 chars)");
    CHECK(r.apply("[chat] A: B: secret") == "[chat] A: (9 chars)");
    CHECK(r.apply("[chat] nocolon") == "[chat] (7 chars)");
    CHECK(r.apply("h \"console: hi all\"") == "h \"(15 chars)\"");
    // addresses: public ones keep two numbers
    CHECK(r.apply("joining 83.12.200.7:28960") == "joining 83.12.x.x:28960");
    CHECK(r.apply("from 127.0.0.1:5 10.0.0.2 172.20.1.1 192.168.0.9 169.254.3.3 0.0.0.0") == "from 127.0.0.1:5 10.0.0.2 172.20.1.1 192.168.0.9 169.254.3.3 0.0.0.0");
    CHECK(r.apply("172.32.1.1 version 1.2.3.4.5 v0.3.0 300.1.1.1") == "172.32.x.x version 1.2.3.4.5 v0.3.0 300.1.1.1");
    CHECK(r.apply("a8.8.8.8b") == "a8.8.x.xb");
}

// The capture of this process: fds 1 and 2 first point to files that stand for the terminal.
static void capture(const stdfs::path& root) {
    const stdfs::path dir = root / "logs";
    const stdfs::path outFile = root / "orig-stdout.txt", errFile = root / "orig-stderr.txt";
    const int savedOut = dup(1), savedErr = dup(2);
    std::fflush(stdout);
    const int o = ::open(outFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644), e = ::open(errFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(o, 1);
    dup2(e, 2);
    ::close(o);
    ::close(e);
    const char* home = std::getenv("HOME");
    const std::string homeDir = home ? home : "/nonexistent-home";

    CaptureOptions opt;
    opt.dir = dir;
    opt.prefix = "iw2server";
    opt.program = "iw2server";
    opt.events = true;
    opt.crashDir = (root / "reports").string();
    opt.crashPrefixes = {"iw2server-"};
    opt.traceNotes = {"IW2_NET_TRACE=2"};
    // An earlier session and a newer crash report: the header names it.
    stdfs::create_directories(dir);
    stdfs::create_directories(root / "reports");
    writeFile(dir / "iw2server-20200101-000000-28960.log", "old");
    stdfs::last_write_time(dir / "iw2server-20200101-000000-28960.log", stdfs::file_time_type::clock::now() - std::chrono::hours(2));
    writeFile(root / "reports" / "iw2server-2026-11-13-101010.ips", "{}");
    writeFile(root / "reports" / "OpenIW2-2026-11-13-101010.ips", "{}");
    CHECK(startCapture(opt));
    CHECK(captureActive() && startCapture(opt)); // a second start keeps the first
    std::printf("plain line from printf %s/Documents/main\n", homeDir.c_str());
    std::cout << "rcon s3cret status" << std::endl;
    std::cerr << "script error: something" << std::endl;
    std::printf("partial ");
    std::fflush(stdout);
    std::printf("line joined\n");
    std::printf("%s\n", std::string(20000, 'L').c_str()); // a long line is cut at 16 KB
    std::fflush(stdout);
    setEventWindowForTest(400);
    for (int i = 0; i < 45; ++i) event("player", "stance " + std::to_string(i));
    event("life", "level mp_toujane dm");
    usleep(450 * 1000);
    event("player", "stance after");
    setEventWindowForTest(10000);
    Event("net") << "connect slot " << 3;
    resetRingForTest(4096);
    trace("net", "NETTRACE frame t=1 late=0");
    Trace("pred") << "predtrace rt=" << 5;
    const std::string dump = dumpTrace("command");
    CHECK(!dump.empty() && stdfs::exists(dump));
    CHECK(dumpTrace("disconnect").empty());   // the automatic dumps: at most one per 10 s (the command one counts)
    CHECK(dumpTrace("script error").empty());
    CHECK(!dumpTrace("command").empty());     // the command is never limited
    drain();
    CHECK(renameSession("28960"));
    std::printf("after rename\n");
    const stdfs::path log = logPath();
    stopCapture();
    std::printf("after stop\n"); // goes to the original stdout only
    std::fflush(stdout);
    dup2(savedOut, 1);
    dup2(savedErr, 2);

    const std::string text = readFile(log), out = readFile(outFile), err = readFile(errFile);
    CHECK(log.filename().string().find("iw2server-") == 0 && log.filename().string().find("-28960.log") != std::string::npos);
    CHECK(text.rfind("OpenIW2 log, iw2server ", 0) == 0);
    CHECK(contains(text, "\nbinary: ") && contains(text, "\nsystem: ") && contains(text, "\nlog: ") && contains(text, "\nflags: "));
    CHECK(contains(text, "\ntraces: IW2_NET_TRACE=2\n"));
    CHECK(contains(text, "previous session: crash report iw2server-2026-11-13-101010.ips") && !contains(text, "OpenIW2-2026-11-13"));
    // lines: prefix, redaction, stderr marker, joined partial line
    CHECK(contains(text, "] plain line from printf ~/Documents/main\n"));
    CHECK(home == nullptr || !contains(text, homeDir + "/Documents"));
    CHECK(contains(text, "] rcon *** status\n") && !contains(text, "s3cret"));
    CHECK(contains(text, "]! script error: something\n"));
    CHECK(contains(text, "] partial line joined\n"));
    CHECK(contains(text, "] " + std::string(16384, 'L') + "\n") && contains(text, "] " + std::string(20000 - 16384, 'L') + "\n"));
    CHECK(contains(text, "] after rename\n") && !contains(text, "after stop"));
    // events: 40 per window, then one suppressed note
    CHECK(contains(text, "] [ev] player stance 39\n") && !contains(text, "stance 40\n") && !contains(text, "stance 44\n"));
    CHECK(contains(text, "] [ev] player 5 lines suppressed\n") && contains(text, "] [ev] player stance after\n"));
    CHECK(contains(text, "] [ev] life level mp_toujane dm\n") && contains(text, "] [ev] net connect slot 3\n"));
    // traces go to the ring, not the file
    CHECK(!contains(text, "NETTRACE frame") && !contains(text, "predtrace"));
    CHECK(contains(text, "trace dump (command): "));
    // the format of a line: [mm:ss.mmm] text
    {
        const size_t p = text.find("plain line");
        const size_t b = text.rfind('\n', p) + 1;
        const std::string pre = text.substr(b, p - b);
        CHECK(pre.size() == 12 && pre[0] == '[' && pre[3] == ':' && pre[6] == '.' && pre[10] == ']' && pre[11] == ' ');
    }
    // the pass-through: the original bytes, unchanged and unprefixed, on the right descriptor
    CHECK(contains(out, "plain line from printf " + homeDir + "/Documents/main\n") && contains(out, "rcon s3cret status\n"));
    CHECK(contains(out, "partial line joined\n") && contains(out, "after stop\n") && !contains(out, "[ev]") && !contains(out, "script error"));
    CHECK(contains(err, "script error: something\n") && !contains(err, "plain line"));
    CHECK(!contains(out, "NETTRACE") && !contains(out, "OpenIW2 log,"));
    // the dump
    const std::string d = readFile(dump);
    CHECK(d.rfind("trace dump (command) of iw2server-", 0) == 0 && contains(d, "] NETTRACE frame t=1 late=0\n") && contains(d, "] predtrace rt=5\n"));
    CHECK(stdfs::path(dump).filename().string().find("-trace-") != std::string::npos && parseLogName(stdfs::path(dump).filename().string()));
    CHECK(countOf(ringContents(), "NETTRACE frame") == 1);

    // Without a capture a trace line goes to stdout as before, and into the ring.
    {
        const stdfs::path f = root / "plain.txt";
        std::fflush(stdout);
        const int fd = ::open(f.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644), keep = dup(1);
        dup2(fd, 1);
        ::close(fd);
        trace("frame", "frametrace us=1");
        std::fflush(stdout);
        dup2(keep, 1);
        ::close(keep);
        CHECK(readFile(f) == "frametrace us=1\n" && contains(ringContents(), "] frametrace us=1\n"));
    }

    // A second capture with a small cap: one note, then only errors, warnings and lifecycle lines, up to the hard cap.
    {
        const stdfs::path capDir = root / "caplogs";
        const int keepOut = dup(1), keepErr = dup(2);
        const int devnull = ::open("/dev/null", O_WRONLY);
        dup2(devnull, 1);
        dup2(devnull, 2);
        ::close(devnull);
        CaptureOptions c2;
        c2.dir = capDir;
        c2.prefix = "openiw2";
        c2.program = "openiw2";
        c2.fileCap = 2000;
        c2.hardCap = 3000;
        CHECK(startCapture(c2));
        for (int i = 0; i < 40; ++i) std::printf("filler line %02d .......................................\n", i);
        std::printf("a Warning: kept\n");
        std::printf("plain dropped\n");
        std::fflush(stdout);
        event("life", "round end");
        event("player", "stance prone");
        std::cerr << "stderr kept" << std::endl;
        drain(); // the two pipes are read in turn: order across them only after a drain
        for (int i = 0; i < 80; ++i) std::printf("error line %02d past the hard cap ...........\n", i);
        const stdfs::path capLog = logPath();
        stopCapture();
        dup2(keepOut, 1);
        dup2(keepErr, 2);
        ::close(keepOut);
        ::close(keepErr);
        const std::string t = readFile(capLog);
        CHECK(countOf(t, "log capped at") == 1);
        CHECK(contains(t, "] a Warning: kept\n") && !contains(t, "plain dropped") && contains(t, "] [ev] life round end\n"));
        CHECK(!contains(t, "stance prone") && contains(t, "]! stderr kept\n"));
        CHECK(!contains(t, "filler line 39") && contains(t, "error line 00") && !contains(t, "error line 79"));
        CHECK(t.size() <= 3000 + 200);
    }
    ::close(savedOut);
    ::close(savedErr);
}

static void switches(const stdfs::path& root) {
    ::unsetenv("IW2_PRED_TRACE");
    ::unsetenv("IW2_BOB_TRACE");
    ::setenv("IW2_FRAME_TRACE", "7", 1);
    int n = 0;
    const auto notes = applyTraceSwitches("IW2_PRED_TRACE, IW2_BOB_TRACE=2\nIW2_FRAME_TRACE=1 IW2_MAIN=/x # comment IW2_VM_TRACE\nIW2_HOME_HACK=1", &n);
    CHECK(n == 3);
    CHECK(std::getenv("IW2_PRED_TRACE") && std::string(std::getenv("IW2_PRED_TRACE")) == "1");
    CHECK(std::getenv("IW2_BOB_TRACE") && std::string(std::getenv("IW2_BOB_TRACE")) == "2");
    CHECK(std::string(std::getenv("IW2_FRAME_TRACE")) == "7"); // the environment wins
    CHECK(!std::getenv("IW2_VM_TRACE") && !std::getenv("IW2_HOME_HACK"));
    CHECK(std::string(std::getenv("IW2_MAIN") ? std::getenv("IW2_MAIN") : "") != "/x");
    bool ignored = false, fromEnv = false;
    for (const auto& s : notes) ignored = ignored || s == "ignored IW2_MAIN=/x (not a trace switch)", fromEnv = fromEnv || s == "IW2_FRAME_TRACE=7 (from the environment)";
    CHECK(ignored && fromEnv && tracesSwitchedOn());
    const stdfs::path cfg = root / "settings.cfg";
    writeFile(cfg, "mainPath=/m\r\ntrace=IW2_NET_TRACE=2 IW2_PRED_TRACE\r\nother=1\n");
    CHECK(traceLineOf(cfg) == "IW2_NET_TRACE=2 IW2_PRED_TRACE");
    CHECK(traceLineOf(root / "missing.cfg").empty());
    ::unsetenv("IW2_PRED_TRACE");
    ::unsetenv("IW2_BOB_TRACE");
    ::unsetenv("IW2_FRAME_TRACE");
}

// The child of fatalPath: a capture, two lines, a trace line, then the signal.
static int crashChild(const char* dir) {
    CaptureOptions o;
    o.dir = dir;
    o.prefix = "openiw2";
    o.program = "openiw2";
    if (!startCapture(o)) return 3;
    setFatalExitForTest(true);
    resetRingForTest(1 << 16);
    trace("kc", "kctrace last frame");
    std::printf("last line before the crash\n");
    std::fprintf(stderr, "stderr before the crash\n");
    std::raise(SIGSEGV);
    return 4;
}

static void fatalPath(const char* self, const stdfs::path& root) {
    const stdfs::path dir = root / "crash";
    stdfs::create_directories(dir);
    const std::string d = dir.string();
    const char* argv[] = {self, "--crash-child", d.c_str(), nullptr};
    pid_t pid = 0;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    CHECK(posix_spawn(&pid, self, &fa, nullptr, const_cast<char**>(argv), environ) == 0);
    posix_spawn_file_actions_destroy(&fa);
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGSEGV);
    std::string log, crashTrace;
    for (const auto& f : filesIn(dir)) {
        const auto n = parseLogName(f);
        if (n && !n->trace) log = readFile(dir / f);
        if (n && n->trace) crashTrace = f;
    }
    CHECK(contains(log, "] last line before the crash\n") && contains(log, "]! stderr before the crash\n"));
    CHECK(contains(log, "! fatal: signal 11 (SIGSEGV): the log ends here"));
    CHECK(log.find("last line before the crash") < log.find("fatal: signal"));
    CHECK(crashTrace.size() > 16 && crashTrace.compare(crashTrace.size() - 16, 16, "-trace-crash.log") == 0);
    CHECK(contains(readFile(dir / crashTrace), "kctrace last frame"));
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--crash-child") == 0) return crashChild(argv[2]);
    reportFd = dup(2);
    const stdfs::path root = stdfs::temp_directory_path() / ("iw2_test_log_" + std::to_string(getpid()));
    stdfs::remove_all(root);
    stdfs::create_directories(root);
    gating();
    names(root);
    redaction();
    fatalPath(argv[0], root);
    switches(root);
    capture(root);
    stdfs::remove_all(root);
    if (failures) {
        dprintf(reportFd, "%d log check(s) failed\n", failures);
        return 1;
    }
    dprintf(reportFd, "log tests passed\n");
    return 0;
}
