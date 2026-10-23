#include "fdulang/frontend.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fdulang {
namespace {

constexpr std::size_t max_source_bytes = 16 * 1024 * 1024;
constexpr std::size_t max_tokens = 200000;
constexpr std::size_t max_ast_nodes = 100000;
constexpr std::size_t max_depth = 256;
constexpr std::int64_t largest_magnitude = 2147483648LL;

bool is_digit(char ch) { return ch >= '0' && ch <= '9'; }
bool is_name_start(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}
bool is_name_part(char ch) { return is_name_start(ch) || is_digit(ch); }
bool is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
           ch == '\f' || ch == '\v';
}
bool has_bom(const std::string& source) {
    return source.size() >= 3 &&
           static_cast<unsigned char>(source[0]) == 0xef &&
           static_cast<unsigned char>(source[1]) == 0xbb &&
           static_cast<unsigned char>(source[2]) == 0xbf;
}

SourceSpan span_at(const std::string& source, std::size_t offset) {
    Position position;
    if (has_bom(source) && offset >= 3) position.offset = 3;
    while (position.offset < offset) {
        const char ch = source[position.offset++];
        if (ch == '\r') {
            if (position.offset < offset && source[position.offset] == '\n')
                ++position.offset;
            ++position.line;
            position.column = 1;
        } else if (ch == '\n') {
            ++position.line;
            position.column = 1;
        } else {
            ++position.column;
        }
    }
    SourceSpan result{position, position};
    if (offset < source.size()) {
        ++result.end.offset;
        ++result.end.column;
    }
    return result;
}

void validate_utf8(const std::string& source) {
    for (std::size_t i = 0; i < source.size();) {
        const std::size_t start = i;
        const auto first = static_cast<unsigned char>(source[i++]);
        if (first <= 0x7f) continue;
        std::uint32_t value = 0;
        std::uint32_t minimum = 0;
        std::size_t remaining = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            value = first & 0x1f; minimum = 0x80; remaining = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            value = first & 0x0f; minimum = 0x800; remaining = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            value = first & 0x07; minimum = 0x10000; remaining = 3;
        } else {
            throw Diagnostic("lex_error", span_at(source, start), "invalid UTF-8 sequence");
        }
        if (remaining > source.size() - i)
            throw Diagnostic("lex_error", span_at(source, start), "truncated UTF-8 sequence");
        for (std::size_t j = 0; j < remaining; ++j) {
            const auto next = static_cast<unsigned char>(source[i++]);
            if ((next & 0xc0) != 0x80)
                throw Diagnostic("lex_error", span_at(source, start), "invalid UTF-8 continuation");
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            throw Diagnostic("lex_error", span_at(source, start), "invalid UTF-8 code point");
    }
}

TokenKind keyword_kind(const std::string& name) {
    if (name == "fn") return TokenKind::Fn;
    if (name == "let") return TokenKind::Let;
    if (name == "input") return TokenKind::Input;
    if (name == "if") return TokenKind::If;
    if (name == "else") return TokenKind::Else;
    if (name == "while") return TokenKind::While;
    if (name == "return") return TokenKind::Return;
    if (name == "halt") return TokenKind::Halt;
    if (name == "abort") return TokenKind::Abort;
    return TokenKind::Identifier;
}

class Lexer {
public:
    explicit Lexer(const std::string& source) : source_(source) {
        if (source.size() > max_source_bytes)
            throw Diagnostic("frontend_limit", {}, "source exceeds 16 MiB");
        validate_utf8(source);
        if (has_bom(source_)) position_.offset = 3;
    }

    std::vector<Token> run() {
        std::vector<Token> result;
        for (;;) {
            skip_trivia();
            const Position start = position_;
            if (at_end()) {
                result.push_back(Token{TokenKind::End, "", {start, start}});
                return result;
            }
            if (result.size() >= max_tokens)
                throw Diagnostic("frontend_limit", {start, start}, "token limit exceeded");
            TokenKind kind;
            const char ch = peek();
            if (is_name_start(ch)) {
                advance();
                while (!at_end() && is_name_part(peek())) advance();
                kind = keyword_kind(source_.substr(start.offset, position_.offset - start.offset));
            } else if (is_digit(ch)) {
                advance();
                while (!at_end() && is_digit(peek())) advance();
                if (!at_end() && is_name_start(peek())) {
                    while (!at_end() && is_name_part(peek())) advance();
                    throw Diagnostic("lex_error", {start, position_}, "invalid numeric token");
                }
                kind = TokenKind::Number;
            } else {
                advance();
                switch (ch) {
                case '(': kind = TokenKind::LParen; break;
                case ')': kind = TokenKind::RParen; break;
                case '{': kind = TokenKind::LBrace; break;
                case '}': kind = TokenKind::RBrace; break;
                case ',': kind = TokenKind::Comma; break;
                case ';': kind = TokenKind::Semicolon; break;
                case '+': kind = TokenKind::Plus; break;
                case '-': kind = TokenKind::Minus; break;
                case '=': kind = consume('=') ? TokenKind::Equal : TokenKind::Assign; break;
                case '<': kind = consume('=') ? TokenKind::LessEqual : TokenKind::Less; break;
                case '>': kind = consume('=') ? TokenKind::GreaterEqual : TokenKind::Greater; break;
                case '!':
                    if (!consume('='))
                        throw Diagnostic("lex_error", {start, position_}, "expected '=' after '!'");
                    kind = TokenKind::NotEqual;
                    break;
                default:
                    throw Diagnostic("lex_error", {start, position_}, "unexpected source character");
                }
            }
            result.push_back(Token{kind,
                source_.substr(start.offset, position_.offset - start.offset), {start, position_}});
        }
    }

private:
    const std::string& source_;
    Position position_;

    bool at_end() const { return position_.offset >= source_.size(); }
    char peek(std::size_t ahead = 0) const {
        const auto index = position_.offset + ahead;
        return index < source_.size() ? source_[index] : '\0';
    }
    void advance() {
        const char ch = source_[position_.offset++];
        if (ch == '\r') {
            if (!at_end() && peek() == '\n') ++position_.offset;
            ++position_.line;
            position_.column = 1;
        } else if (ch == '\n') {
            ++position_.line;
            position_.column = 1;
        } else {
            ++position_.column;
        }
    }
    bool consume(char ch) {
        if (at_end() || peek() != ch) return false;
        advance();
        return true;
    }
    void skip_trivia() {
        for (;;) {
            while (!at_end() && is_space(peek())) advance();
            if (peek() == '/' && peek(1) == '/') {
                advance(); advance();
                while (!at_end() && peek() != '\r' && peek() != '\n') advance();
            } else if (peek() == '/' && peek(1) == '*') {
                const Position start = position_;
                advance(); advance();
                while (!at_end() && !(peek() == '*' && peek(1) == '/')) advance();
                if (at_end())
                    throw Diagnostic("lex_error", {start, position_}, "unterminated block comment");
                advance(); advance();
            } else {
                return;
            }
        }
    }
};

bool is_comparison(TokenKind kind) {
    return kind == TokenKind::Less || kind == TokenKind::LessEqual ||
           kind == TokenKind::Greater || kind == TokenKind::GreaterEqual ||
           kind == TokenKind::Equal || kind == TokenKind::NotEqual;
}
CompareOp comparison_op(TokenKind kind) {
    switch (kind) {
    case TokenKind::Less: return CompareOp::Less;
    case TokenKind::LessEqual: return CompareOp::LessEqual;
    case TokenKind::Greater: return CompareOp::Greater;
    case TokenKind::GreaterEqual: return CompareOp::GreaterEqual;
    case TokenKind::Equal: return CompareOp::Equal;
    case TokenKind::NotEqual: return CompareOp::NotEqual;
    default: throw std::logic_error("not a comparison token");
    }
}

class DepthGuard {
public:
    DepthGuard(std::size_t& depth, const SourceSpan& span) : depth_(depth) {
        if (depth_ >= max_depth)
            throw Diagnostic("frontend_limit", span, "syntax nesting exceeds 256");
        ++depth_;
    }
    ~DepthGuard() { --depth_; }
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;
private:
    std::size_t& depth_;
};

class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens) : tokens_(tokens) {}

    AstProgram run(const std::string& source_name) {
        AstProgram result;
        result.source_name = source_name;
        if (peek().kind == TokenKind::End) fail("expected a function definition");
        while (peek().kind != TokenKind::End) result.functions.push_back(function());
        return result;
    }

private:
    const std::vector<Token>& tokens_;
    std::size_t index_ = 0;
    std::size_t nesting_ = 0;
    std::size_t node_count_ = 0;
    std::unordered_map<const Expr*, std::size_t> expression_depths_;

    const Token& peek() const { return tokens_[index_]; }
    const Token& previous() const { return tokens_[index_ - 1]; }
    const Token& take() { return tokens_[index_++]; }
    bool accept(TokenKind kind) {
        if (peek().kind != kind) return false;
        ++index_;
        return true;
    }
    const Token& expect(TokenKind kind, const char* message) {
        if (peek().kind != kind) fail(message);
        return take();
    }
    [[noreturn]] void fail(const std::string& message) const {
        throw Diagnostic("syntax_error", peek().span, message);
    }
    void count_node(const SourceSpan& span) {
        if (++node_count_ > max_ast_nodes)
            throw Diagnostic("frontend_limit", span, "AST node limit exceeded");
    }
    std::unique_ptr<Expr> new_expr(ExprKind kind, const Token& token) {
        count_node(token.span);
        auto result = std::make_unique<Expr>();
        result->kind = kind;
        result->span = token.span;
        result->operation_span = token.span;
        return result;
    }
    std::unique_ptr<Expr> finish(std::unique_ptr<Expr> value) {
        std::size_t depth = 1;
        if (value->left) depth = std::max(depth, expression_depths_.at(value->left.get()) + 1);
        if (value->right) depth = std::max(depth, expression_depths_.at(value->right.get()) + 1);
        for (const auto& argument : value->arguments)
            depth = std::max(depth, expression_depths_.at(argument.get()) + 1);
        if (depth > max_depth)
            throw Diagnostic("frontend_limit", value->span, "expression tree depth exceeds 256");
        expression_depths_[value.get()] = depth;
        return value;
    }
    std::unique_ptr<Stmt> new_stmt(StmtKind kind, const Token& token) {
        count_node(token.span);
        auto result = std::make_unique<Stmt>();
        result->kind = kind;
        result->span = token.span;
        return result;
    }
    void end_statement(Stmt& statement) {
        statement.span.end = expect(TokenKind::Semicolon, "expected ';'").span.end;
    }

    AstFunction function() {
        const Token start = expect(TokenKind::Fn, "expected 'fn'");
        const Token name = expect(TokenKind::Identifier, "expected function name");
        AstFunction result;
        result.name = name.text;
        result.span.begin = start.span.begin;
        expect(TokenKind::LParen, "expected '(' after function name");
        if (!accept(TokenKind::RParen)) {
            do {
                const Token parameter = expect(TokenKind::Identifier, "expected parameter name");
                result.parameters.push_back(Parameter{parameter.text, parameter.span, -1});
            } while (accept(TokenKind::Comma));
            expect(TokenKind::RParen, "expected ')' after parameters");
        }
        result.body = block();
        result.span.end = result.body->span.end;
        return result;
    }

    std::unique_ptr<Stmt> block() {
        DepthGuard guard(nesting_, peek().span);
        auto result = new_stmt(StmtKind::Block, expect(TokenKind::LBrace, "expected '{'"));
        while (peek().kind != TokenKind::RBrace) {
            if (peek().kind == TokenKind::End) fail("expected '}' before end of source");
            result->statements.push_back(statement());
        }
        result->span.end = take().span.end;
        return result;
    }

    std::unique_ptr<Stmt> if_statement() {
        DepthGuard guard(nesting_, peek().span);
        auto result = new_stmt(StmtKind::If, expect(TokenKind::If, "expected 'if'"));
        expect(TokenKind::LParen, "expected '(' after 'if'");
        result->expression = expression();
        expect(TokenKind::RParen, "expected ')' after condition");
        result->body = block();
        result->span.end = result->body->span.end;
        if (accept(TokenKind::Else)) {
            result->alternative = peek().kind == TokenKind::If ? if_statement() : block();
            result->span.end = result->alternative->span.end;
        }
        return result;
    }

    std::unique_ptr<Stmt> statement() {
        const Token start = peek();
        switch (start.kind) {
        case TokenKind::LBrace: return block();
        case TokenKind::If: return if_statement();
        case TokenKind::Let: {
            take();
            auto result = new_stmt(StmtKind::Let, start);
            result->name = expect(TokenKind::Identifier, "expected variable name").text;
            expect(TokenKind::Assign, "expected '=' in declaration");
            result->expression = expression();
            end_statement(*result);
            return result;
        }
        case TokenKind::Identifier: {
            take();
            if (accept(TokenKind::Assign)) {
                auto result = new_stmt(StmtKind::Assign, start);
                result->name = start.text;
                result->expression = expression();
                end_statement(*result);
                return result;
            }
            if (peek().kind != TokenKind::LParen) fail("expected assignment or function call");
            auto result = new_stmt(StmtKind::Call, start);
            result->expression = call(start);
            end_statement(*result);
            return result;
        }
        case TokenKind::Input: {
            take();
            auto result = new_stmt(StmtKind::Input, start);
            expect(TokenKind::LParen, "expected '(' after 'input'");
            result->name = expect(TokenKind::Identifier, "expected input target variable").text;
            expect(TokenKind::RParen, "expected ')' after input target");
            end_statement(*result);
            return result;
        }
        case TokenKind::While: {
            take();
            auto result = new_stmt(StmtKind::While, start);
            expect(TokenKind::LParen, "expected '(' after 'while'");
            result->expression = expression();
            expect(TokenKind::RParen, "expected ')' after condition");
            result->body = block();
            result->span.end = result->body->span.end;
            return result;
        }
        case TokenKind::Return: {
            take();
            auto result = new_stmt(StmtKind::Return, start);
            result->expression = expression();
            end_statement(*result);
            return result;
        }
        case TokenKind::Halt:
        case TokenKind::Abort: {
            take();
            auto result = new_stmt(start.kind == TokenKind::Halt ? StmtKind::Halt : StmtKind::Abort, start);
            end_statement(*result);
            return result;
        }
        case TokenKind::Semicolon:
            take();
            return new_stmt(StmtKind::Empty, start);
        default: fail("expected a statement");
        }
    }

    std::unique_ptr<Expr> expression() {
        auto result = sum();
        if (is_comparison(peek().kind)) {
            const Token op = take();
            auto compare = new_expr(ExprKind::Compare, op);
            compare->comparison = comparison_op(op.kind);
            compare->left = std::move(result);
            compare->right = sum();
            compare->span = {compare->left->span.begin, compare->right->span.end};
            result = finish(std::move(compare));
            if (is_comparison(peek().kind)) fail("comparison operators cannot be chained");
        }
        return result;
    }
    std::unique_ptr<Expr> sum() {
        auto result = unary();
        while (peek().kind == TokenKind::Plus || peek().kind == TokenKind::Minus) {
            const Token op = take();
            auto binary = new_expr(ExprKind::Binary, op);
            binary->operation = op.text[0];
            binary->left = std::move(result);
            binary->right = unary();
            binary->span = {binary->left->span.begin, binary->right->span.end};
            result = finish(std::move(binary));
        }
        return result;
    }
    std::unique_ptr<Expr> unary() {
        DepthGuard guard(nesting_, peek().span);
        if (peek().kind == TokenKind::Plus || peek().kind == TokenKind::Minus) {
            const Token op = take();
            auto result = new_expr(ExprKind::Unary, op);
            result->operation = op.text[0];
            result->left = unary();
            result->span.end = result->left->span.end;
            return finish(std::move(result));
        }
        return primary();
    }
    std::unique_ptr<Expr> primary() {
        if (accept(TokenKind::LParen)) {
            const Position begin = previous().span.begin;
            auto result = expression();
            result->span.end = expect(TokenKind::RParen, "expected ')'").span.end;
            result->span.begin = begin;
            return result;
        }
        if (peek().kind == TokenKind::Number) {
            const Token number = take();
            auto result = new_expr(ExprKind::Integer, number);
            for (char ch : number.text) {
                const int digit = ch - '0';
                if (result->integer > (largest_magnitude - digit) / 10)
                    throw Diagnostic("literal_range_error", number.span, "integer literal magnitude is too large");
                result->integer = result->integer * 10 + digit;
            }
            return finish(std::move(result));
        }
        if (peek().kind == TokenKind::Identifier) {
            const Token name = take();
            if (peek().kind == TokenKind::LParen) return call(name);
            auto result = new_expr(ExprKind::Variable, name);
            result->name = name.text;
            return finish(std::move(result));
        }
        fail("expected an integer expression");
    }
    std::unique_ptr<Expr> call(const Token& name) {
        auto result = new_expr(ExprKind::Call, name);
        result->name = name.text;
        expect(TokenKind::LParen, "expected '(' after function name");
        if (!accept(TokenKind::RParen)) {
            do { result->arguments.push_back(expression()); } while (accept(TokenKind::Comma));
            expect(TokenKind::RParen, "expected ')' after arguments");
        }
        result->span.end = previous().span.end;
        return finish(std::move(result));
    }
};

class Resolver {
public:
    explicit Resolver(AstProgram& program) : program_(program) {}

    void run() {
        for (std::size_t i = 0; i < program_.functions.size(); ++i) {
            const auto& function = program_.functions[i];
            if (!function_names_.emplace(function.name, static_cast<int>(i)).second)
                throw Diagnostic("name_error", function.span, "duplicate function '" + function.name + "'");
        }
        const auto entry = function_names_.find("main");
        if (entry == function_names_.end())
            throw Diagnostic("entry_error", {}, "a zero-parameter main function is required");
        program_.entry_function = entry->second;
        const auto& main_function = program_.functions[static_cast<std::size_t>(entry->second)];
        if (!main_function.parameters.empty())
            throw Diagnostic("entry_error", main_function.span, "main must have no parameters");
        for (std::size_t i = 0; i < program_.functions.size(); ++i) resolve_function(static_cast<int>(i));

        std::sort(calls_.begin(), calls_.end(), [](const CallRecord& a, const CallRecord& b) {
            return a.expression->operation_span.begin.offset < b.expression->operation_span.begin.offset;
        });
        for (std::size_t i = 0; i < calls_.size(); ++i) {
            auto& record = calls_[i];
            record.expression->call_id = static_cast<int>(i + 1);
            program_.call_sites.push_back(StaticSite{static_cast<int>(i + 1), record.function,
                                                    record.expression->operation_span});
        }
        std::sort(aborts_.begin(), aborts_.end(), [](const AbortRecord& a, const AbortRecord& b) {
            return a.statement->span.begin.offset < b.statement->span.begin.offset;
        });
        for (std::size_t i = 0; i < aborts_.size(); ++i) {
            auto& record = aborts_[i];
            record.statement->abort_id = static_cast<int>(i + 1);
            program_.abort_sites.push_back(StaticSite{static_cast<int>(i + 1), record.function,
                                                     record.statement->span});
        }
    }

private:
    struct CallRecord { Expr* expression; int function; };
    struct AbortRecord { Stmt* statement; int function; };
    AstProgram& program_;
    std::map<std::string, int> function_names_;
    std::vector<std::map<std::string, int>> scopes_;
    std::set<std::string> declared_names_;
    AstFunction* function_ = nullptr;
    int function_index_ = -1;
    std::vector<CallRecord> calls_;
    std::vector<AbortRecord> aborts_;

    void resolve_function(int index) {
        function_index_ = index;
        function_ = &program_.functions[static_cast<std::size_t>(index)];
        declared_names_.clear();
        scopes_.clear();
        scopes_.emplace_back();
        for (auto& parameter : function_->parameters) {
            check_new_name(parameter.name, parameter.span);
            const int slot = static_cast<int>(function_->variables.size());
            parameter.variable = slot;
            function_->variables.push_back(Variable{parameter.name, parameter.span, true, false});
            declared_names_.insert(parameter.name);
            scopes_.back().emplace(parameter.name, slot);
        }
        resolve_block(*function_->body);
    }
    void check_new_name(const std::string& name, const SourceSpan& span) const {
        if (declared_names_.count(name))
            throw Diagnostic("name_error", span, "duplicate variable or parameter '" + name + "'");
    }
    int lookup(const std::string& name, const SourceSpan& span) const {
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
            const auto found = scope->find(name);
            if (found != scope->end()) return found->second;
        }
        throw Diagnostic("name_error", span, "variable '" + name + "' is not visible here");
    }
    void require(Expr& expression, ExprCategory category, const char* message) {
        resolve_expression(expression);
        if (expression.category != category)
            throw Diagnostic("category_error", expression.span, message);
    }
    void resolve_block(Stmt& block) {
        scopes_.emplace_back();
        for (auto& statement : block.statements) resolve_statement(*statement, &block.scope_variables);
        scopes_.pop_back();
    }
    void resolve_statement(Stmt& statement, std::vector<int>* block_variables) {
        switch (statement.kind) {
        case StmtKind::Block:
            resolve_block(statement);
            break;
        case StmtKind::Let: {
            check_new_name(statement.name, statement.span);
            require(*statement.expression, ExprCategory::Integer, "declaration requires an integer");
            const int slot = static_cast<int>(function_->variables.size());
            statement.variable = slot;
            function_->variables.push_back(Variable{statement.name, statement.span, false, false});
            declared_names_.insert(statement.name);
            scopes_.back().emplace(statement.name, slot);
            if (block_variables) block_variables->push_back(slot);
            break;
        }
        case StmtKind::Assign:
            statement.variable = lookup(statement.name, statement.span);
            require(*statement.expression, ExprCategory::Integer, "assignment requires an integer");
            break;
        case StmtKind::Input:
            statement.variable = lookup(statement.name, statement.span);
            break;
        case StmtKind::Call:
            require(*statement.expression, ExprCategory::Integer, "function call must return an integer");
            break;
        case StmtKind::Return:
            require(*statement.expression, ExprCategory::Integer, "return requires an integer");
            break;
        case StmtKind::If:
        case StmtKind::While:
            require(*statement.expression, ExprCategory::Condition, "if/while requires a comparison condition");
            resolve_statement(*statement.body, nullptr);
            if (statement.alternative) resolve_statement(*statement.alternative, nullptr);
            break;
        case StmtKind::Abort:
            aborts_.push_back(AbortRecord{&statement, function_index_});
            break;
        case StmtKind::Halt:
        case StmtKind::Empty:
            break;
        }
    }
    void resolve_expression(Expr& expression) {
        expression.category = ExprCategory::Integer;
        switch (expression.kind) {
        case ExprKind::Integer:
            if (expression.integer > 2147483647LL)
                throw Diagnostic("literal_range_error", expression.operation_span,
                                 "2147483648 is only allowed directly under unary minus");
            break;
        case ExprKind::Variable:
            expression.variable = lookup(expression.name, expression.operation_span);
            break;
        case ExprKind::Unary:
            // Parentheses have no AST node. This exception is a literal form,
            // not constant folding of ordinary source-language arithmetic.
            if (expression.operation == '-' && expression.left->kind == ExprKind::Integer &&
                expression.left->integer == largest_magnitude) {
                expression.kind = ExprKind::Integer;
                expression.integer = -largest_magnitude;
                expression.left.reset();
                break;
            }
            require(*expression.left, ExprCategory::Integer, "unary arithmetic requires an integer");
            break;
        case ExprKind::Binary:
            require(*expression.left, ExprCategory::Integer, "arithmetic requires integer operands");
            require(*expression.right, ExprCategory::Integer, "arithmetic requires integer operands");
            break;
        case ExprKind::Compare:
            require(*expression.left, ExprCategory::Integer, "comparison requires integer operands");
            require(*expression.right, ExprCategory::Integer, "comparison requires integer operands");
            expression.category = ExprCategory::Condition;
            break;
        case ExprKind::Call: {
            const auto function = function_names_.find(expression.name);
            if (function == function_names_.end())
                throw Diagnostic("name_error", expression.operation_span,
                                 "unknown function '" + expression.name + "'");
            expression.function = function->second;
            const auto& target = program_.functions[static_cast<std::size_t>(function->second)];
            if (expression.arguments.size() != target.parameters.size())
                throw Diagnostic("arity_error", expression.operation_span,
                                 "wrong argument count for '" + expression.name + "'");
            for (auto& argument : expression.arguments)
                require(*argument, ExprCategory::Integer, "function arguments must be integers");
            calls_.push_back(CallRecord{&expression, function_index_});
            break;
        }
        }
    }
};

} // namespace

std::vector<Token> tokenize(const std::string& source) {
    return Lexer(source).run();
}

AstProgram parse_and_check(const std::string& source_name, const std::string& source) {
    const auto tokens = tokenize(source);
    auto program = Parser(tokens).run(source_name);
    Resolver(program).run();
    return program;
}

} // namespace fdulang
