#include <fcntl.h>
#include <iostream>
#include <libpcache.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <args.hxx>

#include "commands.hxx"
#include "errors.hxx"
#include "runner.hxx"

static pcache_configuration make_config(uint32_t page_size, uint32_t max_pages, const char *policy_str) {
    pcache_configuration config;
    config.page_size = page_size;
    config.max_pages = max_pages;
    config.id_size   = 16;

    if (policy_str && strcmp(policy_str, "fifo") == 0) {
        config.capacity_policy = PCACHE_CAPACITY_FIFO;
    } else {
        config.capacity_policy = PCACHE_CAPACITY_FIXED;
    }
    return config;
}

static void create_volume(const char *db_path,
                          const char *data_path,
                          uint32_t    page_size,
                          uint32_t    max_pages,
                          const char *policy_str,
                          bool        prealloc_db,
                          bool        prealloc_data,
                          bool        durable) {
    pcache_configuration config = make_config(page_size, max_pages, policy_str);

    pcache_file_pair paths;
    paths.database_path = db_path;
    paths.data_path     = data_path;

    pcache_create_error create_err;
    int                 sqlite_err = 0;
    int                 posix_err  = 0;
    pcache_create(&paths, &config, prealloc_db, prealloc_data, &create_err, &sqlite_err, &posix_err);

    if (create_err != PCACHE_CREATE_OK) {
        print_error(static_cast<int>(create_err), sqlite_err, posix_err);
        return;
    }

    std::cout << "Volume created successfully.\n";
    (void)durable;
}

static bool open_volume(const char *db_path, const char *data_path, repl_context *ctx) {
    pcache_file_pair paths;
    paths.database_path = db_path;
    paths.data_path     = data_path;

    pcache_open_error open_err;
    int               sqlite_err = 0;
    int               posix_err  = 0;
    pcache_handle     handle     = pcache_open(&paths, &open_err, &sqlite_err, &posix_err);

    if (handle == 0) {
        print_error(static_cast<int>(open_err), sqlite_err, posix_err);
        return false;
    }

    ctx->handle = handle;
    ctx->open   = true;

    repl_run(ctx);

    pcache_close_error close_err;
    pcache_close(handle, &close_err, NULL, NULL);
    return true;
}

bool open_volume_with_ctx(const char *db_path, const char *data_path, repl_context *ctx) {
    return open_volume(db_path, data_path, ctx);
}

void CreateCmd(args::Subparser &parser) {
    args::HelpFlag               help(parser, "help", "Show this help", {'h', "help"});
    args::ValueFlag<std::string> db_path(parser, "DB_PATH", "Path to the SQLite index", {'d'});
    args::ValueFlag<std::string> data_path(parser, "DATA_PATH", "Path to the data file", {'D'});
    args::ValueFlag<uint32_t>    page_size(parser, "PAGE_SIZE", "Page size in bytes", {"page-size"});
    args::ValueFlag<uint32_t>    max_pages(parser, "MAX_PAGES", "Maximum number of pages", {"max-pages"});
    args::ValueFlag<std::string> policy(parser, "POLICY", "Capacity policy: fixed or fifo", {"policy"});
    args::Flag prealloc_db(parser, "preallocate-database", "Preallocate database", {"preallocate-database"});
    args::Flag prealloc_data(parser, "preallocate-datafile", "Preallocate data file", {"preallocate-datafile"});
    args::Flag durable(parser, "durable", "Wait for fsync", {'s', "durable"});

    try {
        parser.Parse();
    } catch (const args::SubparserError &e) {
        return;
    }

    const char *db   = db_path.Get().c_str();
    const char *data = data_path.Get().c_str();
    uint32_t    ps   = page_size.Get();
    uint32_t    mp   = max_pages.Get();
    const char *pol  = policy.Get().c_str();

    create_volume(db, data, ps, mp, pol, prealloc_db, prealloc_data, durable);
}

void OpenCmd(args::Subparser &parser) {
    args::HelpFlag               help(parser, "help", "Show this help", {'h', "help"});
    args::ValueFlag<std::string> db_path(parser, "DB_PATH", "Path to the SQLite index", {'d'});
    args::ValueFlag<std::string> data_path(parser, "DATA_PATH", "Path to the data file", {'D'});
    args::ValueFlag<std::string> commands_file(
        parser, "FILE", "Path to a command script to execute", {'c', "commands"});
    args::Flag debug_lexer(parser, "debug-lexer", "Enable lexer debugging", {"debug-lexer"});

    try {
        parser.Parse();
    } catch (const args::SubparserError &e) {
        return;
    }

    const char *db   = db_path.Get().c_str();
    const char *data = data_path.Get().c_str();
    const char *cmds = commands_file.Get().c_str();

    repl_context ctx;
    ctx.commands_file = commands_file.Get().empty() ? nullptr : cmds;
    g_lexer_debug = debug_lexer;

    if (!open_volume_with_ctx(db, data, &ctx)) {
        exit(1);
    }
}
