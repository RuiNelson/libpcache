#include <iostream>
#include <libpcache.h>

#include "cmd_utils.hxx"
#include "errors.hxx"
#include "file_utils.hxx"
#include "repl.hxx"

bool cmd_delete(repl_context *ctx, const command &cmd) {
    if (cmd.args.size() < 1) {
        std::cerr << "Usage: delete <id_file> [--wipe] [--durable]\n";
        return true;
    }

    auto id_buf = read_id_file(cmd.args[0]);
    if (id_buf.empty())
        return false;

    pcache_delete_error del_err;
    int                 sqlite_err = 0, posix_err = 0;
    pcache_delete_page(ctx->handle,
                       id_buf.data(),
                       has_flag(cmd, "--wipe"),
                       has_flag(cmd, "--durable"),
                       &del_err,
                       &sqlite_err,
                       &posix_err);

    if (del_err != PCACHE_DELETE_OK) {
        print_error(static_cast<int>(del_err), sqlite_err, posix_err);
        return false;
    }

    std::cout << "Page deleted.\n";
    return print_ok(), true;
}