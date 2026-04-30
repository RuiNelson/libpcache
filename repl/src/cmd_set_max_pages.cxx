#include <iostream>
#include <libpcache.h>

#include "cmd_utils.hxx"
#include "errors.hxx"
#include "repl.hxx"

bool cmd_set_max_pages(repl_context *ctx, const command &cmd) {
    if (cmd.args.size() < 1) {
        std::cerr << "Usage: set_max_pages <max_pages> [--durable]\n";
        return true;
    }

    uint32_t new_max = static_cast<uint32_t>(std::stoul(cmd.args[0]));

    pcache_set_max_pages_error smp_err;
    int                        sqlite_err = 0, posix_err = 0;
    pcache_set_max_pages(ctx->handle, new_max, has_flag(cmd, "--durable"), &smp_err, &sqlite_err, &posix_err);

    if (smp_err != PCACHE_SET_MAX_PAGES_OK) {
        print_error(static_cast<int>(smp_err), sqlite_err, posix_err);
        return false;
    }

    std::cout << "Max pages set to " << cmd.args[0] << ".\n";
    return print_ok(), true;
}