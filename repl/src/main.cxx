#include <args.hxx>
#include <iostream>
#include <libpcache.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "commands.hxx"
#include "lexer.hxx"


static args::ArgumentParser g_parser("pcache_repl", "A REPL for libpcache");

static args::HelpFlag g_help(g_parser, "help", "Show this help", {'h', "help"});

static args::Command g_create(g_parser, "create", "Create a new volume", &CreateCmd);
static args::Command g_open(g_parser, "open", "Open an existing volume", &OpenCmd);

int main(int argc, char **argv) {
    try {
        if (!g_parser.ParseCLI(argc, argv)) {
            return 1;
        }
    } catch (const args::Help &e) {
        std::cout << g_parser;
        return 0;
    } catch (const args::Error &e) {
        fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    return 0;
}
