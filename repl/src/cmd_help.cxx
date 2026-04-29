#include <iostream>

#include "cmd_utils.hxx"
#include "errors.hxx"
#include "file_utils.hxx"
#include "repl.hxx"

bool cmd_help(repl_context *ctx, const command &cmd) {
    (void)ctx;
    (void)cmd;
    std::cout << "Available commands:\n";
    std::cout << "  help\n";
    std::cout << "  put <id_file> <page_file> [--fail-if-exists] [--durable]\n";
    std::cout << "  get <id_file> <output_file>\n";
    std::cout << "  check <id_file>\n";
    std::cout << "  delete <id_file> [--wipe] [--durable]\n";
    std::cout << "  space\n";
    std::cout << "  inspect\n";
    std::cout << "  set_max_pages <max_pages> [--durable]\n";
    std::cout << "  defragment [--shrink] [--durable]\n";
    std::cout << "  close\n";
    return print_ok(), true;
}