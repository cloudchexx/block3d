#include <block3d/block3d.hpp>

#include <iostream>

int main() {
    auto v = block3d::version();
    std::cout << "block3d " << v.major << "." << v.minor << "." << v.patch << "\n";
    return 0;
}
