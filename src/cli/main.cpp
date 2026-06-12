#include "core/version.hpp"

#include <iostream>

int main() {
    std::cout << "BisonDB " << bisondb::version() << "\n";
    return 0;
}
