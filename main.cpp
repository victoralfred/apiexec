#include "core/engine.hpp"
#include "adapters/generic_rest.hpp"
#include "adapters/registry.hpp"

#include <iostream>

int main(int argc, char* argv[]) {
    std::cout << "apiexec v0.1.0 — vendor-agnostic streaming execution engine\n";

    if (argc < 2) {
        std::cout << "Usage: apiexec <config.json>\n";
        std::cout << "  Registered adapters:";
        for (const auto& name : apiexec::Registry::instance().adapter_names()) {
            std::cout << " " << name;
        }
        std::cout << "\n";
        return 0;
    }

    return 0;
}
