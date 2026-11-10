// The server console window's model (src/netgame/consoleview.h); implemented from the spec (docs/spec/mp-ui.md).
#include "netgame/consoleview.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace iw2::netgame {

void ConsoleView::append(const std::string& text) {
    size_t p = 0;
    while (p < text.size()) {
        const size_t nl = text.find('\n', p);
        const std::string part = text.substr(p, (nl == std::string::npos ? text.size() : nl) - p);
        if (open_) lines_.back() += part;
        else lines_.push_back(part);
        open_ = nl == std::string::npos;
        if (nl == std::string::npos) break;
        p = nl + 1;
    }
    if (lines_.size() > maxLines_) lines_.erase(lines_.begin(), lines_.begin() + std::ptrdiff_t(lines_.size() - maxLines_));
    scroll_ = std::min(scroll_, lines_.size());
}

std::vector<std::string> ConsoleView::visible(size_t rows) const {
    const size_t endLine = lines_.size() - std::min(scroll_, lines_.size());
    const size_t first = endLine > rows ? endLine - rows : 0;
    return {lines_.begin() + std::ptrdiff_t(first), lines_.begin() + std::ptrdiff_t(endLine)};
}

void ConsoleView::insert(const std::string& text) {
    std::string clean;
    for (char c : text)
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f) clean += c;
    input_.insert(cursor_, clean);
    cursor_ += clean.size();
}

void ConsoleView::backspace() {
    if (cursor_ == 0) return;
    input_.erase(--cursor_, 1);
}

void ConsoleView::del() {
    if (cursor_ < input_.size()) input_.erase(cursor_, 1);
}

void ConsoleView::left() { cursor_ = cursor_ > 0 ? cursor_ - 1 : 0; }
void ConsoleView::right() { cursor_ = std::min(cursor_ + 1, input_.size()); }
void ConsoleView::home() { cursor_ = 0; }
void ConsoleView::end() { cursor_ = input_.size(); }

void ConsoleView::historyUp() {
    if (historyPos_ == 0) return;
    input_ = history_[--historyPos_];
    cursor_ = input_.size();
}

void ConsoleView::historyDown() {
    if (historyPos_ >= history_.size()) return;
    ++historyPos_;
    input_ = historyPos_ < history_.size() ? history_[historyPos_] : std::string();
    cursor_ = input_.size();
}

void ConsoleView::scroll(int n) {
    const long long v = static_cast<long long>(scroll_) + n;
    scroll_ = static_cast<size_t>(std::clamp<long long>(v, 0, static_cast<long long>(lines_.size())));
}

std::optional<std::string> ConsoleView::enter() {
    std::string cmd = input_;
    input_.clear();
    cursor_ = 0;
    scroll_ = 0;
    if (cmd.find_first_not_of(' ') == std::string::npos) {
        historyPos_ = history_.size();
        return std::nullopt;
    }
    if (history_.empty() || history_.back() != cmd) history_.push_back(cmd);
    historyPos_ = history_.size();
    append((open_ ? "\n]" : "]") + cmd + "\n");
    return cmd;
}

bool StdoutTee::start() {
    if (savedFd_ >= 0) return true;
    int fds[2];
    if (::pipe(fds) != 0) return false;
    std::fflush(stdout);
    savedFd_ = ::dup(1);
    if (savedFd_ < 0 || ::dup2(fds[1], 1) < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        if (savedFd_ >= 0) ::close(savedFd_);
        savedFd_ = -1;
        return false;
    }
    ::close(fds[1]);
    readFd_ = fds[0];
    reader_ = std::thread([this] {
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(readFd_, buf, sizeof buf);
            if (n <= 0) break;
            for (ssize_t off = 0; off < n;) {
                const ssize_t w = ::write(savedFd_, buf + off, size_t(n - off));
                if (w <= 0) break;
                off += w;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            pending_.append(buf, size_t(n));
        }
    });
    return true;
}

void StdoutTee::stop() {
    if (savedFd_ < 0) return;
    std::fflush(stdout);
    ::dup2(savedFd_, 1); // closes the pipe's write end: the reader sees the end of file
    if (reader_.joinable()) reader_.join();
    ::close(readFd_);
    ::close(savedFd_);
    readFd_ = savedFd_ = -1;
}

std::string StdoutTee::take() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(pending_, std::string());
}

} // namespace iw2::netgame
