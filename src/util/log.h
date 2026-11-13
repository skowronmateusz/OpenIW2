#pragma once
// Session logs, event lines and the trace recorder (docs/logging.md). An openiw2 addition: POSIX only, no SDL, used by
// iw2view and iw2server (which stays SDL-free and builds on Linux).
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace iw2::log {

// ---- Build and platform facts ----
std::string buildVersion(); // git describe of the build (IW2_VERSION), "" when unknown
std::string buildType();    // CMAKE_BUILD_TYPE
std::string executablePath();
bool inAppBundle(std::string_view executablePath); // the path contains ".app/Contents/"

// ---- Gating (docs/logging.md "When a log is written") ----
// A 0.x version: matches ^v?0\.
bool preRelease(std::string_view version);
// env: IW2_LOG (nullptr when unset), cvar: the value of --cvar iw2_log (nullptr when not given). "0" is off, any
// other non-empty value on; the environment wins over the cvar; else on for a 0.x version in an app bundle.
bool loggingWanted(std::string_view version, bool inBundle, const char* env, const char* cvar);
// The event lines: on with the log unless IW2_LOG_EVENTS / iw2_logEvents say 0; IW2_LOG_EVENTS=1 also without it.
bool eventsWanted(bool logging, const char* env, const char* cvar);
// The value of "--cvar <name>=<value>" (the last one) in a command line, nullopt when absent.
std::optional<std::string> cvarArgument(int argc, char** argv, std::string_view name);

// ---- Folder and names ----
std::filesystem::path defaultLogDir(); // IW2_LOG_DIR, else the platform default (docs/logging.md)
// "<prefix>-YYYYMMDD-HHMMSS" in local time, "-<suffix>" appended when not empty.
std::string sessionStem(std::string_view prefix, std::time_t t, std::string_view suffix = {});
struct LogName {
    std::string prefix;  // "openiw2" or "iw2server"
    std::string session; // the session's stem (without a trace part and ".log")
    bool trace = false;
};
// A managed log file name (docs/logging.md "Files"), nullopt for any other name.
std::optional<LogName> parseLogName(std::string_view fileName);
bool legacyLogName(std::string_view fileName); // openiw2.log, openiw2-1.log .. openiw2-9.log, openiw2.log.1
// Deletes legacy names, then all but the newest `keepPerPrefix` sessions of each prefix (with their trace files), then
// the oldest sessions while the managed files exceed capBytes. `current` (a session stem) is never deleted.
void pruneLogs(const std::filesystem::path& dir, const std::string& current, size_t keepPerPrefix = 10,
               uint64_t capBytes = 100ull << 20);

// ---- Redaction (docs/logging.md "Redaction") ----
class Redactor {
public:
    explicit Redactor(std::string home = {}) : home_(std::move(home)) {}
    std::string apply(std::string_view line) const;

private:
    std::string home_;
};

// ---- Capture ----
struct CaptureOptions {
    std::filesystem::path dir;
    std::string prefix;          // "openiw2" or "iw2server"
    std::string program;         // shown in the header ("openiw2", "iw2server")
    bool events = true;
    uint64_t fileCap = 20ull << 20;
    uint64_t hardCap = 24ull << 20;
    std::string crashDir;        // where crash reports are looked for ("" = ~/Library/Logs/DiagnosticReports on macOS)
    std::vector<std::string> crashPrefixes; // "OpenIW2-", "iw2server-"
    std::vector<std::string> traceNotes;    // lines for the header (trace switches applied, warnings)
};
// Redirects fds 1 and 2 into the log (docs/logging.md "Capture") and installs the fatal handlers. False (nothing
// changed) when the folder or the file cannot be made. Calling it twice keeps the first capture.
bool startCapture(const CaptureOptions& options);
bool captureActive();
std::filesystem::path logPath();  // "" without a capture
std::string sessionName();        // the session stem ("" without a capture)
// Appends "-<suffix>" to the session's name (server: the bound port) and renames the file.
bool renameSession(const std::string& suffix);
// The start of iw2view / iw2server: the gating (IW2_LOG, --cvar iw2_log, a 0.x version in an app bundle), then the trace
// switches of `traceText` (a settings.cfg trace= line) and of traces.txt in the log folder, then the capture with the
// events per IW2_LOG_EVENTS / --cvar iw2_logEvents. Returns whether the capture runs.
bool startProgramLog(int argc, char** argv, const std::string& prefix, const std::string& program,
                     const std::vector<std::string>& crashPrefixes, const std::string& traceText = {});
// Whether startProgramLog would capture (for the caller's preparation, e.g. reading settings.cfg).
bool programLogWanted(int argc, char** argv);
// Flushes, restores fds 1 and 2, drains the pipes and stops the reader. Also run at exit.
void stopCapture();
// A header line "<key>: <value>" written straight into the file (redacted); nothing without a capture.
void headerLine(std::string_view key, std::string_view value);
// Waits until the reader has written everything already in the pipes (tests, event ordering).
void drain();

// ---- Event lines ----
bool eventsEnabled();
void setEventsEnabled(bool on);
// "[ev] <category> <text>" into the log file (or to stdout without a capture). Budget: 40 lines per category per 10 s.
void event(std::string_view category, std::string_view text);
// Collects a line with <<, writes it as an event at the end of the statement.
class Event {
public:
    explicit Event(const char* category) : category_(category) {}
    ~Event() { event(category_, out_.str()); }
    template <class T> Event& operator<<(const T& v) { out_ << v; return *this; }
    Event& operator<<(std::ios_base& (*manip)(std::ios_base&)) { out_ << manip; return *this; }

private:
    const char* category_;
    std::ostringstream out_;
};

// ---- Trace recorder (docs/logging.md "Trace recorder") ----
// One trace line (no newline needed): to the ring and to `out` (stdout or stderr, as the trace printed before). With the
// log file on it goes to the descriptor the program was started with (not /dev/null) instead, never into the file.
void trace(std::string_view category, std::string_view line, std::FILE* out = stdout);
// printf-style trace line.
void tracef(const char* category, const char* format, ...) __attribute__((format(printf, 2, 3)));
// Collects a trace line with <<, written at the end of the statement.
class Trace {
public:
    explicit Trace(const char* category, std::FILE* out = stdout) : category_(category), out_(out) {}
    ~Trace();
    template <class T> Trace& operator<<(const T& v) { text_ << v; return *this; }
    Trace& operator<<(std::ios_base& (*manip)(std::ios_base&)) { text_ << manip; return *this; } // std::hex, std::dec
    std::ostringstream& stream() { return text_; }

private:
    const char* category_;
    std::FILE* out_;
    std::ostringstream text_;
};
// Writes the ring's last 60 s to <session>-trace-HHMMSS.log. reason: "command" (never rate limited), "disconnect",
// "script error", "exit". Returns the file written, "" when nothing was (empty ring, rate limit).
std::string dumpTrace(std::string_view reason);
// Parses trace switches ("IW2_NET_TRACE=2 IW2_PRED_TRACE"), sets the known ones with setenv (an existing value wins)
// and returns notes for the header ("set IW2_NET_TRACE=2", "ignored IW2_FOO"). `applied` counts the set ones.
std::vector<std::string> applyTraceSwitches(std::string_view text, int* applied = nullptr);
// The trace= line of a settings.cfg ("" when none).
std::string traceLineOf(const std::filesystem::path& settingsFile);
bool tracesSwitchedOn(); // applyTraceSwitches set at least one

// ---- Test hooks ----
void resetRingForTest(size_t bytes);
std::string ringContents();
void setFatalExitForTest(bool on);       // the fatal handler ends with _exit(128 + signal), no system crash report
void setEventWindowForTest(int64_t ms);  // the event budget's window (default 10000)

} // namespace iw2::log
