// Exercise CLI error transport across the actual writer process boundary.
#include <iostream>
#include <string>

int main() {
    const std::string block(4096, 'x');
    for (int i=0;i<3072;++i) std::cout.write(block.data(),block.size());
    std::cout << "\nFINAL_WRITER_ERROR: deliberate integration test failure\n";
    return 17;
}
