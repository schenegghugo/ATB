//
// creature_gen.cpp — Emit data/creatures.json from the compiled bestiary.
// Keeps the canonical file and makeDefaultCreatures() in lockstep (CI diffs it).
//
//   tb_creature_gen [out_path=data/creatures.json] [version=1.0.0]
//
#include "core/Creatures.h"
#include "data/CreatureJson.h"
#include "data/Sha256.h"

#include <cstdio>
#include <fstream>
#include <string>

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "data/creatures.json";
    const std::string version = argc > 2 ? argv[2] : "1.0.0";

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
