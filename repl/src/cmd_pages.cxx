#include <iostream>
#include <libpcache.h>

#include "cmd_utils.hxx"
#include "errors.hxx"
#include "repl.hxx"

bool cmd_pages(repl_context *ctx, const command &cmd) {
    (void)ctx;
    (void)cmd;
    pcache_inspect_page_count_error cnt_err;
    int                             sqlite_err = 0;
    pcache_page_count               count      = pcache_inspect_page_count(ctx->handle, &cnt_err, &sqlite_err);

    if (cnt_err != PCACHE_INSPECT_PAGE_COUNT_OK) {
        print_error(static_cast<int>(cnt_err), sqlite_err, 0);
        return false;
    }

    std::cout << "Used: " << count.used << ", Free: " << count.free << "\n";
    return print_ok(), true;
}