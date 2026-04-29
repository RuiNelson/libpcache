#pragma once

#include "repl.hxx"

void repl_run(void *ctx_ptr);
bool run_command(repl_context *ctx, const std::string &line);
bool run_script(repl_context *ctx, const std::string &path);