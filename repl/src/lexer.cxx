#include <iostream>
#include <string>
#include <vector>

#include "lexer.hxx"
#include "repl.hxx"

bool g_lexer_debug = false;

enum lex_state { LEX_START, LEX_COMMAND, LEX_FLAG_DASH, LEX_FLAG_VALUE_PREPARE, LEX_FLAG_VALUE, LEX_DONE };

void lexer_tokenize(token_list *list, const std::string &input) {
    list->tokens.clear();
    list->pos = 0;

    lex_state   state = LEX_START;
    std::string lexeme;

    if (g_lexer_debug)
        std::cerr << "LEX: input=[" << input << "] len=" << input.size() << "\n";

    for (size_t i = 0; i <= input.size(); i++) {
        char c        = (i < input.size()) ? input[i] : '\0';
        bool is_space = (c == ' ' || c == '\t');
        bool is_end   = (c == '\0');

        if (g_lexer_debug)
            std::cerr << "LEX: i=" << i << " c=" << (c ? c : '0') << " is_space=" << is_space
                      << " is_end=" << is_end << " state=" << state << " lexeme=[" << lexeme << "]\n";

        switch (state) {
            case LEX_START:
                if (is_end) {
                    state = LEX_DONE;
                } else if (!is_space) {
                    lexeme.clear();
                    if (c == '-') {
                        lexeme += c;
                        state = LEX_FLAG_DASH;
                    } else {
                        lexeme += c;
                        state = LEX_COMMAND;
                    }
                }
                break;

            case LEX_COMMAND:
                if (is_end || is_space) {
                    list->tokens.push_back({token::TOKEN_COMMAND, lexeme, {}});
                    if (g_lexer_debug)
                        std::cerr << "LEX: Pushed TOKEN_COMMAND: [" << lexeme << "]\n";
                    lexeme.clear();
                    state = is_end ? LEX_DONE : LEX_START;
                } else if (c == '=') {
                    list->tokens.push_back({token::TOKEN_FLAG, lexeme, {}});
                    lexeme.clear();
                    state = LEX_FLAG_VALUE_PREPARE;
                } else {
                    lexeme += c;
                }
                break;

            case LEX_FLAG_DASH:
                if (c == '-' && lexeme.size() == 1) {
                    lexeme += c;
                } else if (is_space || is_end) {
                    list->tokens.push_back({token::TOKEN_FLAG, lexeme, {}});
                    lexeme.clear();
                    state = is_end ? LEX_DONE : LEX_START;
                } else if (c == '=') {
                    list->tokens.push_back({token::TOKEN_FLAG, lexeme, {}});
                    lexeme.clear();
                    state = LEX_FLAG_VALUE_PREPARE;
                } else {
                    lexeme += c;
                }
                break;

            case LEX_FLAG_VALUE_PREPARE:
                if (is_space || is_end) {
                    if (!lexeme.empty()) {
                        list->tokens.back().flag_value = lexeme;
                        lexeme.clear();
                    }
                    state = is_end ? LEX_DONE : LEX_START;
                } else {
                    lexeme += c;
                    state = LEX_FLAG_VALUE;
                }
                break;

            case LEX_FLAG_VALUE:
                if (is_end || is_space) {
                    list->tokens.back().flag_value = lexeme;
                    lexeme.clear();
                    state = is_end ? LEX_DONE : LEX_START;
                } else {
                    lexeme += c;
                }
                break;

            case LEX_DONE:
                break;
        }
    }
}

token *lexer_next(token_list *list) {
    if (list->pos >= list->tokens.size())
        return nullptr;
    return &list->tokens[list->pos++];
}

void lexer_reset(token_list *list) {
    list->pos = 0;
}

command parser_parse(token_list *tokens) {
    command cmd;
    lexer_reset(tokens);

    token *t = lexer_next(tokens);
    if (!t || t->type != token::TOKEN_COMMAND)
        return cmd;
    cmd.name = t->lexeme;

    if (g_lexer_debug)
        std::cerr << "LEX: cmd.name=[" << cmd.name << "]\n";

    while ((t = lexer_next(tokens))) {
        if (g_lexer_debug)
            std::cerr << "LEX: Got token type=" << t->type << " lexeme=[" << t->lexeme
                      << "] args.size=" << cmd.args.size() << "\n";
        if (t->type == token::TOKEN_COMMAND && cmd.args.size() >= 2)
            return cmd;
        if (t->type == token::TOKEN_FLAG)
            cmd.flags.push_back(t->lexeme);
        if (t->type == token::TOKEN_STRING || t->type == token::TOKEN_COMMAND)
            cmd.args.push_back(t->lexeme);
    }
    if (g_lexer_debug)
        std::cerr << "LEX: Final args.size=" << cmd.args.size() << "\n";
    return cmd;
}