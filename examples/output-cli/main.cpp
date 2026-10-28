#include <iostream>

#include "vtb/output/output_cli.hpp"
#include "vtb/output/output_manager.hpp"

int main() {
    vtb::OutputManager manager;
    vtb::OutputCli cli(manager, std::cin, std::cout);
    return cli.run();
}
