// json-floats.cpp -- the host's answers for toJSON's floats (D7): Nix prints a
// float in JSON with nlohmann/json, so this prints each with nlohmann/json.
// Reads IEEE-754 binary64 bit patterns, one hexadecimal per line, and writes
// nlohmann::json(x).dump() for each, one per line. scripts/float-vectors.sh
// runs it; target/tests/float_test.c checks float.c's f_json against it.
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <nlohmann/json.hpp>

int main()
{
    std::string line;
    while (std::getline(std::cin, line)) {
        std::uint64_t u = std::stoull(line, nullptr, 16);
        double d;
        std::memcpy(&d, &u, 8);
        std::cout << nlohmann::json(d).dump() << "\n";
    }
    return 0;
}
