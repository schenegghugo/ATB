//
// catalog_gen.cpp — Scaffold / validate the data catalog.
//
// data/catalog.json is the SOURCE OF TRUTH (hand-editable). This tool has two
// modes:
//   tb_catalog_gen --check [path=data/catalog.json]   load + validate (CI gate)
//   tb_catalog_gen [--force] [out_path] [version]     (re)scaffold from the
//                                                      compiled makeDefaultCatalog()
//
// Scaffold mode REFUSES to overwrite an existing file (it's the source of truth);
// pass --force to clobber it (bootstrap only — this discards hand edits).
//
#include "core/Spells.h"
#include "data/CatalogJson.h"
#include "data/Sha256.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    // Validation mode: the committed data file is canonical, so CI checks that it
    // loads + validates (rather than diffing it against the compiled seed).
    if (argc > 1 && std::string(argv[1]) == "--check") {
        const std::string path = argc > 2 ? argv[2] : "data/catalog.json";
        const tb::CatalogLoad load = tb::loadCatalogFromFile(path);
        if (!load.ok) {
            std::fprintf(stderr, "catalog: '%s' is invalid:\n", path.c_str());
            for (const std::string& e : load.errors) std::fprintf(stderr, "  - %s\n", e.c_str());
            return 1;
        }
        std::printf("ok: %s — %zu spells, v%s\n", path.c_str(), load.catalog.all().size(),
                    load.version.c_str());
        return 0;
    }

    // Scaffold mode: parse a --force flag out of the positional args.
    bool force = false;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--force") force = true;
        else pos.push_back(a);
    }
    const std::string path = !pos.empty() ? pos[0] : "data/catalog.json";
    const std::string version = pos.size() > 1 ? pos[1] : "1.0.0";

    if (!force && std::ifstream(path).good()) {
        std::fprintf(stderr,
                     "catalog_gen: '%s' already exists — refusing to overwrite (it's the source "
                     "of truth). Pass --force to scaffold anyway (discards hand edits).\n",
                     path.c_str());
        return 1;
    }

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
