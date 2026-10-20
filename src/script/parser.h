// GSC lexer and recursive-descent parser.
#pragma once

#include "script/ast.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace iw2::script {

struct ParseError : std::runtime_error {
    int line;
    ParseError(const std::string& file, int l, const std::string& msg)
        : std::runtime_error(file + ":" + std::to_string(l) + ": " + msg), line(l) {}
};

// Parses one script file. `name` is used in error messages. Throws ParseError.
NodePtr parseScript(std::string_view source, const std::string& name);

// Writes a compact s-expression of the tree (for tests and debugging).
std::string dumpTree(const Node& n);

} // namespace iw2::script
