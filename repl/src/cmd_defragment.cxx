#include <iostream>
#include <libpcache.h>

#include "cmd_utils.hxx"
#include "errors.hxx"
#include "repl.hxx"

bool cmd_defragment(repl_context *ctx, const command &cmd) {
    pcache_defragment_error def_err;
    int                      sqlite_err = 0, posix_err = 0;
    pcache_defragment(ctx->handle, nullptr, nullptr,
                      has_flag(cmd, "--shrink"),
                      has_flag(cmd, "--durable"),
                      &def_err, &sqlite_err, &posix_err);

    if (def_err != PCACHE_DEFRAGMENT_OK) {
        print_error(static_cast<int>(def_err), sqlite_err, posix_err);
        return false;
    }

    std::cout << "Defragmentation complete.\n";
    return print_ok(), true;
}