#include <iostream>
#include <libpcache.h>
#include <vector>

#include "cmd_utils.hxx"
#include "errors.hxx"
#include "file_utils.hxx"
#include "repl.hxx"

bool cmd_get(repl_context *ctx, const command &cmd) {
    if (cmd.args.size() < 2) {
        std::cerr << "Usage: get <id_file> <output_file>\n";
        return true;
    }

    auto id_buf = read_id_file(cmd.args[0]);
    if (id_buf.empty())
        return false;

    pcache_inspect_configuration_error conf_err;
    pcache_configuration               config = pcache_inspect_configuration(ctx->handle, &conf_err);

    std::vector<uint8_t> page_buf(config.page_size);

    pcache_get_error err;
    int              sqlite_err = 0, posix_err = 0;
    pcache_get_page(ctx->handle, id_buf.data(), page_buf.data(), &err, &sqlite_err, &posix_err);

    if (err != PCACHE_GET_OK) {
        print_error(static_cast<int>(err), sqlite_err, posix_err);
        return false;
    }

    if (!write_file(cmd.args[1], page_buf.data(), config.page_size)) {
        std::cerr << "Failed to write output file: " << cmd.args[1] << "\n";
        return false;
    }

    std::cout << "Page retrieved (" << config.page_size << " bytes) to " << cmd.args[1] << ".\n";
    return print_ok(), true;
}