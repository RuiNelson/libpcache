#include <iostream>
#include <libpcache.h>

#include "cmd_utils.hxx"
#include "errors.hxx"
#include "repl.hxx"

bool cmd_inspect(repl_context *ctx, const command &cmd) {
    (void)cmd;
    pcache_inspect_configuration_error err;
    pcache_configuration config = pcache_inspect_configuration(ctx->handle, &err);

    if (err != PCACHE_INSPECT_CONFIGURATION_OK) {
        std::cerr << "inspect failed\n";
        return false;
    }

    const char *policy = (config.capacity_policy == PCACHE_CAPACITY_FIFO) ? "fifo" : "fixed";
    std::cout << "Page size: " << config.page_size << " bytes\n";
    std::cout << "Max pages: " << config.max_pages << "\n";
    std::cout << "Capacity policy: " << policy << "\n";
    std::cout << "ID size: " << config.id_size << " bytes\n";
    return print_ok(), true;
}