#include "cmd_utils.hxx"
#include "errors.hxx"
#include "repl.hxx"

bool cmd_close(repl_context *ctx, const command &cmd) {
    (void)cmd;
    ctx->open = false;
    return print_ok(), true;
}