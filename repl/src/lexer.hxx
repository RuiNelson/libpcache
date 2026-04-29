#pragma once

#include <string>
#include <vector>

extern bool g_lexer_debug;

struct token {
    enum token_type { TOKEN_EOF, TOKEN_COMMAND, TOKEN_STRING, TOKEN_FLAG } type;
    std::string lexeme;
    std::string flag_value;
};

struct token_list {
    std::vector<token> tokens;
    size_t             pos;
};

struct command {
    std::string              name;
    std::vector<std::string> args;
    std::vector<std::string> flags;
};

void lexer_tokenize(token_list *list, const std::string &input);
token *lexer_next(token_list *list);
void lexer_reset(token_list *list);
command parser_parse(token_list *tokens);