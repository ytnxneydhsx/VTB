#pragma once

#include <iosfwd>
#include <map>
#include <string>

#include "vtb/output/output_manager.hpp"

namespace vtb {

class OutputCli {
public:
    OutputCli(OutputManager& manager, std::istream& input, std::ostream& output);
    ~OutputCli() = default;

    OutputCli(const OutputCli&) = delete;
    OutputCli& operator=(const OutputCli&) = delete;
    OutputCli(OutputCli&&) = delete;
    OutputCli& operator=(OutputCli&&) = delete;

    int run();

private:
    void execute_line(const std::string& line, bool& quit);
    void print_help();
    void print_list();
    void stop_active_outputs();

    OutputManager& manager_;
    std::istream& input_;
    std::ostream& output_;
    std::map<std::string, StreamDescriptor> streams_;
};

}  // namespace vtb
