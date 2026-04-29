#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct repl_context {
    int         handle;
    bool        open;
    const char *commands_file;
};