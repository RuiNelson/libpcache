#include <iostream>
#include <libpcache.h>

#include "cmd_utils.hxx"
#include "errors.hxx"
#include "file_utils.hxx"
#include "repl.hxx"

bool cmd_check(repl_context *ctx, const command &cmd) {
    if (cmd.args.size() < 1) {
        std::cerr << "Usage: check <id_file>\n";
        return true;
    }

    auto id_buf = read_id_file(cmd.args[0]);
    if (id_buf.empty())
        return false;

    pcache_check_error chk_err;
    int                sqlite_err = 0;
    bool               exists = pcache_check_page(ctx->handle, id_buf.data(), &chk_err, &sqlite_err);

    if (chk_err != PCACHE_CHECK_OK) {
        print_error(static_cast<int>(chk_err), sqlite_err, 0);
        return false;
    }

    std::cout << "Page " << (exists ? "exists" : "does not exist") << ".\n";
    return print_ok(), true;
}