// iw2script: parse GSC scripts from the game data.
//
//   iw2script <main-dir> check              parse every .gsc and report errors
//   iw2script <main-dir> dump <path.gsc>    print the syntax tree
//   iw2script <main-dir> compile            compile every .gsc with the VM compiler
//   iw2script <main-dir> run <map> [seconds] run a level's main() headless with stub built-ins
//   iw2script <main-dir> builtins           list called functions that no script defines (engine built-ins)

#include "assets/d3dbsp.h"
#include "fs/vfs.h"
#include "script/parser.h"
#include "script/builtins_core.h"
#include "script/vm.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
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
        } else if (cmd == "compile") {
            iw2::script::VM vm([&](const std::string& p) -> std::optional<std::string> {
                auto d = vfs.read(p);
                if (!d) return std::nullopt;
                return std::string(d->begin(), d->end());
            });
            int ok = 0, failed = 0;
            for (const auto& name : vfs.list()) {
                if (name.size() < 4 || name.substr(name.size() - 4) != ".gsc") continue;
                try {
                    vm.loadFile(name);
                    ++ok;
                } catch (const std::exception& e) {
                    if (failed < 25) std::cerr << e.what() << "\n";
                    ++failed;
                }
            }
            std::cout << "total: " << ok << " ok, " << failed << " failed\n";
            return failed ? 1 : 0;
        } else if (cmd == "run" && argc > 3) {
            using namespace iw2::script;
            const std::string map = argv[3];
            const int seconds = argc > 4 ? std::atoi(argv[4]) : 10;
            VM vm([&](const std::string& p) -> std::optional<std::string> {
                auto d = vfs.read(p);
                if (!d) return std::nullopt;
                return std::string(d->begin(), d->end());
            });
            vm.print = [](const std::string& s) { std::cout << "[print] " << s << "\n"; };
            registerCoreBuiltins(vm);
            int32_t nextFx = 0;
            vm.registerBuiltin("loadfx", [&](CallContext&) { return Value(nextFx++); });

            // Entities from the map, with key/values as fields (vectors parsed for origin/angles).
            const auto bspData = vfs.read("maps/" + map + ".d3dbsp");
            if (!bspData) { std::cerr << "map not found\n"; return 1; }
            const iw2::assets::D3dBsp bsp(*bspData);
            std::vector<ObjectPtr> entities;
            for (const auto& e : bsp.entities()) {
                const std::string* cls = e.get("classname");
                auto o = vm.newObject(cls ? *cls : "entity");
                for (const auto& [k, v] : e.fields) {
                    std::string key = k;
                    for (char& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
                    if (key == "origin" || key == "angles") {
                        Vec3 vec{};
                        std::sscanf(v.c_str(), "%f %f %f", &vec[0], &vec[1], &vec[2]);
                        o->set(key, Value(vec));
                    } else {
                        o->set(key, Value(v));
                    }
                }
                entities.push_back(o);
            }
            auto findEnts = [&](const Value& value, const Value& key) {
                std::vector<ObjectPtr> out;
                const std::string k = key.isDefined() ? key.toString() : "targetname";
                for (const auto& o : entities)
                    if (!o->removed && o->get(k) == value) out.push_back(o);
                return out;
            };
            vm.registerBuiltin("getent", [&](CallContext& c) -> Value {
                auto r = findEnts(c.arg(0), c.arg(1));
                if (r.size() > 1) throw std::runtime_error("getent used with more than one entity: " + c.arg(0).toString());
                return r.empty() ? Value() : Value(r[0]);
            });
            vm.registerBuiltin("getentarray", [&](CallContext& c) -> Value {
                auto arr = std::make_shared<Array>();
                int32_t i = 0;
                for (auto& o : findEnts(c.arg(0), c.arg(1))) { ArrayKey k; k.i = i++; arr->set(k, Value(o)); }
                return Value(arr);
            });
            std::map<std::string, std::string> cvars;
            vm.registerBuiltin("getcvar", [&](CallContext& c) { return Value(cvars[c.arg(0).toString()]); });
            vm.registerBuiltin("getcvarint", [&](CallContext& c) { return Value(int32_t(std::atoi(cvars[c.arg(0).toString()].c_str()))); });
            vm.registerBuiltin("getcvarfloat", [&](CallContext& c) { return Value(float(std::atof(cvars[c.arg(0).toString()].c_str()))); });
            vm.registerBuiltin("setcvar", [&](CallContext& c) { cvars[c.arg(0).toString()] = c.arg(1).toString(); return Value(); });
            vm.registerBuiltin("spawn", [&](CallContext& c) {
                auto o = vm.newObject(c.arg(0).toString());
                o->set("classname", c.arg(0));
                o->set("origin", c.arg(1));
                entities.push_back(o);
                return Value(o);
            });

            // Every other built-in the scripts call becomes a counting stub.
            std::map<std::string, int> stubCalls;
            {
                std::set<std::string> defined;
                std::set<std::string> called;
                for (const auto& name : vfs.list()) {
                    if (name.size() < 4 || name.substr(name.size() - 4) != ".gsc") continue;
                    const auto data = vfs.read(name);
                    try {
                        const auto tree = parseScript(std::string_view(reinterpret_cast<const char*>(data->data()), data->size()), name);
                        std::function<void(const Node&)> walk = [&](const Node& n) {
                            if (n.kind == NodeKind::Function) { std::string l = n.text; for (char& ch : l) ch = char(std::tolower(static_cast<unsigned char>(ch))); defined.insert(l); }
                            if (n.kind == NodeKind::Call && !n.isPointerCall && n.children.size() > 1 && n.children[1]->text.find("::") == std::string::npos) {
                                std::string l = n.children[1]->text; for (char& ch : l) ch = char(std::tolower(static_cast<unsigned char>(ch))); called.insert(l);
                            }
                            for (const auto& c : n.children) walk(*c);
                        };
                        walk(*tree);
                    } catch (const std::exception&) {
                    }
                }
                static const std::set<std::string> real = {"getent", "getentarray", "getcvar", "getcvarint", "getcvarfloat", "setcvar", "spawn",
                                                           "isdefined", "isstring", "spawnstruct", "int", "float", "println", "iprintln",
                                                           "assert", "assertex", "assertmsg", "gettime", "getarraykeys"};
                for (const auto& f : called)
                    if (!defined.count(f) && !real.count(f) && !vm.hasBuiltin(f))
                        vm.registerBuiltin(f, [&stubCalls, f](CallContext&) { ++stubCalls[f]; return Value(); });
            }

            vm.level()->set("script", Value(map));
            const auto t0 = std::chrono::steady_clock::now();
            vm.start("maps/" + map + ".gsc", "main");
            for (int64_t ms = 50; ms <= int64_t(seconds) * 1000; ms += 50) vm.runFrame(ms);
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::cout << "simulated " << seconds << " s in " << secs << " s wall time; live threads: " << vm.liveThreadCount()
                      << "; runtime errors: " << vm.errors().size() << "\n";
            std::map<std::string, int> errCount;
            for (const auto& e : vm.errors()) ++errCount[e];
            int shown = 0;
            for (const auto& [e, n] : errCount) if (shown++ < 20) std::cout << "  error x" << n << ": " << e << "\n";
            std::vector<std::pair<int, std::string>> top;
            for (const auto& [f, n] : stubCalls) top.push_back({n, f});
            std::sort(top.rbegin(), top.rend());
            std::cout << "stub built-ins called: " << top.size() << "\n";
            for (size_t i = 0; i < top.size() && i < 40; ++i) std::cout << "  " << top[i].second << " x" << top[i].first << "\n";
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
