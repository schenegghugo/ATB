//
// catalog_gen.cpp — Emit data/catalog.json from the compiled makeDefaultCatalog().
//
// Keeps the canonical content file and the code in lockstep: regenerate after
// changing the default catalog, and CI diffs the committed file against fresh
// output to catch drift.
//
//   tb_catalog_gen [out_path=data/catalog.json] [version=1.0.0]
//
#include "core/Spells.h"
#include "data/CatalogJson.h"
#include "data/Sha256.h"

#include <cstdio>
#include <fstream>
#include <string>

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "data/catalog.json";
    const std::string version = argc > 2 ? argv[2] : "1.0.0";

    const std::string json = tb::serializeCatalog(tb::makeDefaultCatalog(), version);

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "catalog_gen: cannot write %s\n", path.c_str());
        return 1;
    }
    out << json;
    if (!out) {
        std::fprintf(stderr, "catalog_gen: write failed for %s\n", path.c_str());
        return 1;
    }
    std::printf("wrote %s (%zu bytes)\n", path.c_str(), json.size());
    std::printf("sha256 %s\n", tb::sha256Hex(json).c_str());
    return 0;
}
