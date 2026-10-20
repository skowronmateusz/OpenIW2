// GSC abstract syntax tree (docs/spec/gsc.md).
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace iw2::script {

enum class NodeKind {
    // Top level
    File, Include, UsingAnimtree, Function,
    // Statements
    Block, ExprStmt, If, While, For, Switch, Case, Default, Break, Continue, Return, Wait, WaitTillFrameEnd,
    Breakpoint, DevBlock, Empty,
    // Expressions
    Assign,      // text = operator ("=", "+=", ...); children: target, value
    PreIncDec,   // text = "++"/"--"
    PostIncDec,
    Binary,      // text = operator
    Unary,       // text = "!", "-", "~"
    Call,        // children: [callee-object or Empty], function, args...; flags: thread / pointer
    FunctionRef, // ::func or path::func; text = qualified name
    Member,      // children: object; text = field
    Index,       // children: object, index
    Identifier, Integer, Float, String, LocalizedString, Vector, EmptyArray, Undefined, True, False,
    Self, Level, Game, Anim, AnimRef, AnimTree, Size
};

struct Node {
    NodeKind kind;
    std::string text;
    int line = 0;
    bool isThread = false;       // Call: "thread"
    bool isPointerCall = false;  // Call: [[ expr ]](...)
    std::vector<std::unique_ptr<Node>> children;

    Node(NodeKind k, std::string t, int l) : kind(k), text(std::move(t)), line(l) {}
    Node* add(std::unique_ptr<Node> c) {
        children.push_back(std::move(c));
        return children.back().get();
    }
};

using NodePtr = std::unique_ptr<Node>;

const char* nodeKindName(NodeKind k);

} // namespace iw2::script
