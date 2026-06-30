//
// creature_gen.cpp — Scaffold / validate the data bestiary.
//
// data/creatures.json is the SOURCE OF TRUTH (hand-editable). Two modes:
//   tb_creature_gen --check [path=data/creatures.json]   load + validate (CI gate)
//   tb_creature_gen [--force] [out_path] [version]       (re)scaffold from the
//                                                         compiled makeDefaultCreatures()
//
// Scaffold mode REFUSES to overwrite an existing file; pass --force to clobber it
// (bootstrap only — discards hand edits).
//
#include "core/Creatures.h"
#include "data/CreatureJson.h"
#include "data/Sha256.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--check") {
        const std::string path = argc > 2 ? argv[2] : "data/creatures.json";
        const tb::CreatureLoad load = tb::loadCreaturesFromFile(path);
        if (!load.ok) {
            std::fprintf(stderr, "creatures: '%s' is invalid:\n", path.c_str());
            for (const std::string& e : load.errors) std::fprintf(stderr, "  - %s\n", e.c_str());
            return 1;
        }
        std::printf("ok: %s — %zu creatures, v%s\n", path.c_str(), load.creatures.size(),
                    load.version.c_str());
        return 0;
    }

    bool force = false;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--force") force = true;
        else pos.push_back(a);
    }
    const std::string path = !pos.empty() ? pos[0] : "data/creatures.json";
    const std::string version = pos.size() > 1 ? pos[1] : "1.0.0";

    if (!force && std::ifstream(path).good()) {
        std::fprintf(stderr,
                     "creature_gen: '%s' already exists — refusing to overwrite (source of "
                     "truth). Pass --force to scaffold anyway (discards hand edits).\n",
                     path.c_str());
        return 1;
    }

    const std::string json = tb::serializeCreatures(tb::makeDefaultCreatures(), version);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "creature_gen: cannot write %s\n", path.c_str());
        return 1;
    }
    out << json;
    std::printf("wrote %s (%zu bytes)\n", path.c_str(), json.size());
    std::printf("sha256 %s\n", tb::sha256Hex(json).c_str());
    return 0;
}
