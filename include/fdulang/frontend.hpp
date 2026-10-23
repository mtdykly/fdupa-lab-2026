#ifndef FDULANG_FRONTEND_HPP
#define FDULANG_FRONTEND_HPP

#include "fdulang/ast.hpp"
#include <string>
#include <vector>

namespace fdulang {

enum class TokenKind { End, Identifier, Number, Fn, Let, Input, If, Else,
    While, Return, Halt, Abort, LParen, RParen, LBrace, RBrace, Comma,
    Semicolon, Plus, Minus, Assign, Less, LessEqual, Greater, GreaterEqual,
    Equal, NotEqual };
struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;
    SourceSpan span;
};

// Diagnostics use lex_error, syntax_error, name_error, arity_error,
// category_error, literal_range_error, entry_error, or frontend_limit.
std::vector<Token> tokenize(const std::string& source);
AstProgram parse_and_check(const std::string& source_name, const std::string& source);

} // namespace fdulang
#endif
