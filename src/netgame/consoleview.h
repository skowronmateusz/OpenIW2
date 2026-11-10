// The server console window's model (docs/spec/mp-ui.md "Dedicated start"): the scrolling log, the command line with
// its history, and a tee that keeps the process's standard output going to the terminal while the window shows it.
// No drawing here: iw2view draws it (apps/iw2view), the tests drive it headless.
#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace iw2::netgame {

class ConsoleView {
public:
    explicit ConsoleView(size_t maxLines = 4000) : maxLines_(maxLines) {}

    // Log text (any chunks; split at newlines, a trailing partial line stays open for the next chunk).
    void append(const std::string& text);
    const std::vector<std::string>& lines() const { return lines_; }
    // The log lines a window `rows` lines high shows, oldest first (the last `rows` lines above the scroll offset).
    std::vector<std::string> visible(size_t rows) const;

    // Command line editing.
    void insert(const std::string& text); // typed text at the cursor (control characters dropped)
    void backspace();
    void del();
    void left();
    void right();
    void home();
    void end();
    void historyUp();   // the previous command (empty history: nothing)
    void historyDown(); // the next one, past the newest an empty line
    void scroll(int lines); // positive: back into the log; clamped
    // Enter: returns the command (none for an empty line), echoes it into the log as "]<command>", keeps it in the
    // history (not twice in a row) and clears the line.
    std::optional<std::string> enter();

    const std::string& input() const { return input_; }
    size_t cursor() const { return cursor_; }
    size_t scrollOffset() const { return scroll_; }
    const std::vector<std::string>& history() const { return history_; }

private:
    size_t maxLines_;
    std::vector<std::string> lines_;
    bool open_ = false; // the last line has no newline yet
    std::string input_;
    size_t cursor_ = 0;
    std::vector<std::string> history_;
    size_t historyPos_ = 0; // == history_.size(): the line being typed
    size_t scroll_ = 0;
};

// Copies everything written to the standard output (fd 1) into a buffer while still writing it to where fd 1 pointed
// before (the terminal or a log file). POSIX pipe and a reader thread.
class StdoutTee {
public:
    StdoutTee() = default;
    ~StdoutTee() { stop(); }
    StdoutTee(const StdoutTee&) = delete;
    StdoutTee& operator=(const StdoutTee&) = delete;
    bool start();
    void stop();      // restores fd 1
    std::string take(); // the text captured since the last call

private:
    int savedFd_ = -1, readFd_ = -1;
    std::thread reader_;
    std::mutex mutex_;
    std::string pending_;
};

} // namespace iw2::netgame
