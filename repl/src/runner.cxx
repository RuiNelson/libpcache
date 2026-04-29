#include <fstream>
#include <iostream>
#include <readline/history.h>
#include <readline/readline.h>

#include "commands.hxx"
#include "lexer.hxx"
#include "runner.hxx"

bool run_command(repl_context *ctx, const std::string &line) {
    token_list tokens;
    lexer_tokenize(&tokens, line);

    if (tokens.tokens.empty())
        return true;

    command cmd = parser_parse(&tokens);
    if (cmd.name.empty()) {
        std::cerr << "Failed to parse command.\n";
        return true;
    }

    if (cmd.name == "help") return cmd_help(ctx, cmd);
    if (cmd.name == "close") return cmd_close(ctx, cmd);
    if (cmd.name == "put") return cmd_put(ctx, cmd);
    if (cmd.name == "get") return cmd_get(ctx, cmd);
    if (cmd.name == "check") return cmd_check(ctx, cmd);
    if (cmd.name == "delete") return cmd_delete(ctx, cmd);
    if (cmd.name == "space") return cmd_pages(ctx, cmd);
    if (cmd.name == "inspect") return cmd_inspect(ctx, cmd);
    if (cmd.name == "set_max_pages") return cmd_set_max_pages(ctx, cmd);
    if (cmd.name == "defragment") return cmd_defragment(ctx, cmd);

    std::cerr << "Unknown command: " << cmd.name << ". Type 'help' for available commands.\n";
    return true;
}

bool run_script(repl_context *ctx, const std::string &path) {
    std::ifstream f(path);
    if (!f) {
        std::cerr << "Failed to open command file: " << path << "\n";
        return false;
    }

    std::string line;
    while (std::getline(f, line)) {
        if (!ctx->open)
            return true;
        if (!run_command(ctx, line))
            return false;
    }
    return true;
}

void repl_run(void *ctx_ptr) {
    repl_context *ctx = static_cast<repl_context *>(ctx_ptr);

    if (ctx->commands_file) {
        if (!run_script(ctx, ctx->commands_file))
            ctx->open = false;
        return;
    }

    std::cout << "Volume opened. Type 'help' for available commands.\n";

    while (ctx->open) {
        char *line = readline("pcache> ");
        if (!line)
            break;
        if (*line)
            add_history(line);
        run_command(ctx, line);
        free(line);
    }
}