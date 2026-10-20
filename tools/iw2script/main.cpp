// iw2script: parse GSC scripts from the game data.
//
//   iw2script <main-dir> check              parse every .gsc and report errors
//   iw2script <main-dir> dump <path.gsc>    print the syntax tree
//   iw2script <main-dir> builtins           list called functions that no script defines (engine built-ins)

#include "fs/vfs.h"
#include "script/parser.h"

#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: iw2script <main-dir> check | dump <path.gsc>\n";
        return 2;
    }
    try {
        iw2::fs::Vfs vfs;
        vfs.mountDirectory(argv[1]);
        const std::string cmd = argv[2];
        if (cmd == "dump" && argc > 3) {
            const auto data = vfs.read(argv[3]);
            if (!data) { std::cerr << "not found\n"; return 1; }
            const auto tree = iw2::script::parseScript(std::string_view(reinterpret_cast<const char*>(data->data()), data->size()), argv[3]);
            std::cout << iw2::script::dumpTree(*tree) << "\n";
        } else if (cmd == "builtins") {
            std::set<std::string> defined;
            std::map<std::string, std::pair<int, int>> called; // name -> (plain calls, method calls)
            for (const auto& name : vfs.list()) {
                if (name.size() < 4 || name.substr(name.size() - 4) != ".gsc") continue;
                const auto data = vfs.read(name);
                try {
                    const auto tree = iw2::script::parseScript(std::string_view(reinterpret_cast<const char*>(data->data()), data->size()), name);
                    std::function<void(const iw2::script::Node&)> walk = [&](const iw2::script::Node& n) {
                        if (n.kind == iw2::script::NodeKind::Function) defined.insert(n.text);
                        if (n.kind == iw2::script::NodeKind::Call && !n.isPointerCall && n.children.size() > 1) {
                            const std::string& f = n.children[1]->text;
                            if (f.find("::") == std::string::npos) {
                                std::string lower = f;
                                for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
                                auto& cnt = called[lower];
                                (n.children[0]->kind == iw2::script::NodeKind::Empty ? cnt.first : cnt.second)++;
                            }
                        }
                        for (const auto& c : n.children) walk(*c);
                    };
                    walk(*tree);
                } catch (const std::exception&) {
                }
            }
            std::set<std::string> definedLower;
            for (auto d : defined) { for (char& c : d) c = char(std::tolower(static_cast<unsigned char>(c))); definedLower.insert(d); }
            int n = 0;
            for (const auto& [f, c] : called)
                if (!definedLower.count(f)) { std::cout << f << "\t" << c.first << "\t" << c.second << "\n"; ++n; }
            std::cerr << n << " built-ins\n";
        } else if (cmd == "check") {
            int ok = 0, failed = 0, functions = 0;
            for (const auto& name : vfs.list()) {
                if (name.size() < 4 || name.substr(name.size() - 4) != ".gsc") continue;
                const auto data = vfs.read(name);
                try {
                    const auto tree = iw2::script::parseScript(std::string_view(reinterpret_cast<const char*>(data->data()), data->size()), name);
                    for (const auto& c : tree->children) functions += c->kind == iw2::script::NodeKind::Function;
                    ++ok;
                } catch (const std::exception& e) {
                    if (failed < 25) std::cerr << e.what() << "\n";
                    ++failed;
                }
            }
            std::cout << "functions: " << functions << "\ntotal: " << ok << " ok, " << failed << " failed\n";
            return failed ? 1 : 0;
        } else {
            std::cerr << "unknown command\n";
            return 2;
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
