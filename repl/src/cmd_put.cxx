#include <iostream>
#include <libpcache.h>

#include "cmd_utils.hxx"
#include "errors.hxx"
#include "file_utils.hxx"
#include "repl.hxx"

bool cmd_put(repl_context *ctx, const command &cmd) {
    if (cmd.args.size() < 2) {
        std::cerr << "Usage: put <id_file> <page_file> [--fail-if-exists] [--durable]\n";
        return true;
    }

    auto id_buf = read_id_file(cmd.args[0]);
    if (id_buf.empty())
        return false;

    auto page_buf = read_file(cmd.args[1]);
    if (page_buf.empty()) {
        std::cerr << "Failed to read page file: " << cmd.args[1] << "\n";
        return false;
    }

    pcache_put_error err;
    int              sqlite_err = 0, posix_err = 0;
    pcache_put_page(ctx->handle, id_buf.data(), page_buf.data(),
                    has_flag(cmd, "--fail-if-exists"),
                    has_flag(cmd, "--durable"),
                    &err, &sqlite_err, &posix_err);

    if (err != PCACHE_PUT_OK) {
        print_error(static_cast<int>(err), sqlite_err, posix_err);
        return false;
    }

    std::cout << "Page stored successfully.\n";
    return print_ok(), true;
}