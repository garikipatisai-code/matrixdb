// matrixdb — interactive CLI/REPL over the CPU analytical engine. All logic is in matrix_cli.hpp; this is
// just the entry point.
//   clang++ -std=c++20 -O2 matrixdb_cli.cpp -o matrixdb
//   ./matrixdb                  # interactive REPL on stdin
//   ./matrixdb -c "SELECT ..."  # run one line, exit
//   ./matrixdb -f script.sql    # run a file of commands/queries, exit
#include "matrix_cli.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
    CPUMockEngine eng;
    const std::string arg1 = argc >= 2 ? argv[1] : "";
    if (arg1 == "-c" && argc >= 3) {
        std::istringstream in(std::string(argv[2]) + "\n");
        return matrix_repl(in, std::cout, eng);
    }
    if (arg1 == "-f" && argc >= 3) {
        std::ifstream f(argv[2]);
        if (!f) { std::cerr << "matrixdb: cannot open " << argv[2] << "\n"; return 1; }
        return matrix_repl(f, std::cout, eng);
    }
    return matrix_repl(std::cin, std::cout, eng);  // interactive (or piped stdin)
}
