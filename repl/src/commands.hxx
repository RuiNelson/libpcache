#pragma once

#include <args.hxx>
#include <libpcache.h>

#include "lexer.hxx"
#include "repl.hxx"

bool cmd_help(repl_context *ctx, const command &cmd);
bool cmd_close(repl_context *ctx, const command &cmd);
bool cmd_put(repl_context *ctx, const command &cmd);
bool cmd_get(repl_context *ctx, const command &cmd);
bool cmd_check(repl_context *ctx, const command &cmd);
bool cmd_delete(repl_context *ctx, const command &cmd);
bool cmd_pages(repl_context *ctx, const command &cmd);
bool cmd_inspect(repl_context *ctx, const command &cmd);
bool cmd_set_max_pages(repl_context *ctx, const command &cmd);
bool cmd_defragment(repl_context *ctx, const command &cmd);

bool open_volume_with_ctx(const char *db_path, const char *data_path, repl_context *ctx);

void CreateCmd(args::Subparser &parser);
void OpenCmd(args::Subparser &parser);