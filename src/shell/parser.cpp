#include "shell/parser.hpp"

#include "core/json_parser.hpp"
#include "core/query/database.hpp"

#include <cctype>

namespace bisondb::shell {

namespace {

bool isIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}
bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '-';
}

class StatementParser {
  public:
    explicit StatementParser(std::string_view input) : input_(input) {}

    ShellCommand parse() {
        skipWs();
        std::string word = ident("a statement");
        if (word == "help") {
            endOfStatement();
            return ShellCommand{Verb::Help, {}, {}, {}, {}, {}, false};
        }
        if (word == "exit" || word == "quit") {
            endOfStatement();
            return ShellCommand{Verb::Exit, {}, {}, {}, {}, {}, false};
        }
        if (word == "show") {
            skipWs();
            std::size_t at = pos_;
            std::string what = ident("'collections' or 'status'");
            endOfStatement();
            if (what == "collections") {
                return ShellCommand{Verb::ShowCollections, {}, {}, {}, {}, {}, false};
            }
            if (what == "status") {
                return ShellCommand{Verb::ShowStatus, {}, {}, {}, {}, {}, false};
            }
            throw ShellParseError(at, "expected 'collections' or 'status' after 'show'");
        }
        if (word != "db") {
            throw ShellParseError(0, "statements start with 'db.', 'show', 'help', or 'exit'");
        }
        expect('.', "'.' after 'db'");
        std::size_t collAt = pos_;
        std::string coll = ident("a collection name");
        if (!query::Database::isValidCollectionName(coll)) {
            throw ShellParseError(collAt, "invalid collection name '" + coll + "'");
        }
        expect('.', "'.' after the collection name");
        std::size_t methodAt = pos_;
        std::string method = ident("a method name");
        ShellCommand cmd = parseMethod(coll, method, methodAt);
        if (cmd.verb == Verb::Find) {
            parseChainedModifiers(cmd);
        }
        endOfStatement();
        return cmd;
    }

  private:
    ShellCommand parseMethod(const std::string& coll, const std::string& method,
                             std::size_t methodAt) {
        ShellCommand cmd{Verb::Find, coll, {}, {}, {}, {}, false};
        expect('(', "'(' after '" + method + "'");
        auto closeParen = [this] { expect(')', "')'"); };

        if (method == "insertOne") {
            cmd.verb = Verb::InsertOne;
            cmd.args.push_back(jsonArg("a document"));
            closeParen();
        } else if (method == "insertMany") {
            cmd.verb = Verb::InsertMany;
            std::size_t at = posAfterWs();
            Value v = jsonArg("an array of documents");
            if (!v.is<Array>()) {
                throw ShellParseError(at, "insertMany expects an array of documents");
            }
            cmd.args.push_back(std::move(v));
            closeParen();
        } else if (method == "find" || method == "count" || method == "deleteMany") {
            cmd.verb = method == "find" ? Verb::Find
                                        : (method == "count" ? Verb::Count : Verb::DeleteMany);
            skipWs();
            if (!atEnd() && peek() == ')') {
                if (method == "deleteMany") {
                    throw ShellParseError(pos_, "deleteMany requires a filter (use {} for all)");
                }
                cmd.args.push_back(Value(Document{}));
                advance();
            } else {
                cmd.args.push_back(jsonArg("a filter document"));
                closeParen();
            }
        } else if (method == "updateOne") {
            cmd.verb = Verb::UpdateOne;
            cmd.args.push_back(jsonArg("a filter document"));
            expect(',', "',' between filter and update");
            cmd.args.push_back(jsonArg("an update document"));
            closeParen();
        } else if (method == "createIndex" || method == "dropIndex") {
            cmd.verb = method == "createIndex" ? Verb::CreateIndex : Verb::DropIndex;
            std::size_t at = posAfterWs();
            Value v = jsonArg("a field name");
            if (v.is<std::string>()) {
                cmd.field = v.get<std::string>();
            } else if (v.is<Document>() && v.asDocument().size() == 1) {
                // Mongo-style ({field: 1}); the direction is ignored.
                cmd.field = v.asDocument()[0].first;
            } else {
                throw ShellParseError(at, method + " expects \"field\" or {field: 1}");
            }
            closeParen();
        } else if (method == "getIndexes" || method == "drop" || method == "compact") {
            cmd.verb = method == "getIndexes" ? Verb::GetIndexes
                                              : (method == "drop" ? Verb::Drop : Verb::Compact);
            skipWs();
            closeParen();
        } else {
            throw ShellParseError(methodAt, "unknown method '" + method + "'");
        }
        return cmd;
    }

    void parseChainedModifiers(ShellCommand& cmd) {
        while (true) {
            skipWs();
            if (atEnd() || peek() != '.') {
                return;
            }
            advance();
            std::size_t at = pos_;
            std::string mod = ident("'limit', 'skip', or 'explain'");
            expect('(', "'(' after '" + mod + "'");
            if (mod == "explain") {
                if (cmd.explain) {
                    throw ShellParseError(at, "duplicate .explain()");
                }
                cmd.explain = true;
                skipWs();
                expect(')', "')'");
                continue;
            }
            if (mod != "limit" && mod != "skip") {
                throw ShellParseError(at, "unknown modifier '." + mod + "()'");
            }
            std::size_t numAt = posAfterWs();
            Value v = jsonArg("a non-negative integer");
            int64_t n = 0;
            if (v.is<int32_t>()) {
                n = v.get<int32_t>();
            } else if (v.is<int64_t>()) {
                n = v.get<int64_t>();
            } else {
                throw ShellParseError(numAt, "." + mod + "() expects an integer");
            }
            if (n < 0) {
                throw ShellParseError(numAt, "." + mod + "() expects a non-negative integer");
            }
            expect(')', "')'");
            auto& slot = mod == "limit" ? cmd.limit : cmd.skip;
            if (slot.has_value()) {
                throw ShellParseError(at, "duplicate ." + mod + "()");
            }
            slot = n;
        }
    }

    // ---- low-level cursor ----------------------------------------------

    bool atEnd() const { return pos_ >= input_.size(); }
    char peek() const { return input_[pos_]; }
    char advance() { return input_[pos_++]; }
    void skipWs() {
        while (!atEnd() && std::isspace(static_cast<unsigned char>(peek()))) {
            ++pos_;
        }
    }
    std::size_t posAfterWs() {
        skipWs();
        return pos_;
    }

    void expect(char c, const std::string& what) {
        skipWs();
        if (atEnd() || peek() != c) {
            throw ShellParseError(pos_, "expected " + what);
        }
        advance();
    }

    std::string ident(const std::string& what) {
        skipWs();
        if (atEnd() || !isIdentStart(peek())) {
            throw ShellParseError(pos_, "expected " + what);
        }
        std::string out;
        out.push_back(advance());
        while (!atEnd() && isIdentChar(peek())) {
            out.push_back(advance());
        }
        return out;
    }

    void endOfStatement() {
        skipWs();
        if (!atEnd() && peek() == ';') {
            advance();
            skipWs();
        }
        if (!atEnd()) {
            throw ShellParseError(pos_, "unexpected trailing input");
        }
    }

    // Delegates to the relaxed JSON parser, translating its line/column into
    // an absolute statement offset for the caret diagnostic.
    Value jsonArg(const std::string& what) {
        skipWs();
        if (atEnd()) {
            throw ShellParseError(pos_, "expected " + what);
        }
        std::size_t start = pos_;
        try {
            std::size_t consumed = 0;
            JsonParseOptions opts;
            opts.relaxed = true;
            Value v = parseJsonOne(input_.substr(start), consumed, opts);
            pos_ = start + consumed;
            return v;
        } catch (const JsonParseError& e) {
            std::size_t offset = start;
            std::string_view rest = input_.substr(start);
            std::size_t line = 1;
            for (std::size_t i = 0; i < rest.size() && line < e.line(); ++i) {
                ++offset;
                if (rest[i] == '\n') {
                    ++line;
                }
            }
            offset += e.column() - 1;
            // Strip the "JSON parse error at line L, column C: " prefix.
            std::string msg = e.what();
            std::size_t colon = msg.find(": ", msg.find("column"));
            if (colon != std::string::npos) {
                msg = msg.substr(colon + 2);
            }
            throw ShellParseError(offset, msg);
        }
    }

    std::string_view input_;
    std::size_t pos_ = 0;
};

// Shared bracket/string scanner for continuation detection and ';' splitting.
template <typename OnTopLevelSemicolon>
bool scanBalance(std::string_view buffer, OnTopLevelSemicolon onSemicolon) {
    int depth = 0;
    char quote = 0;
    bool escaped = false;
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        char c = buffer[i];
        if (quote != 0) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == quote) {
                quote = 0;
            }
            continue;
        }
        switch (c) {
        case '"':
        case '\'': quote = c; break;
        case '(':
        case '{':
        case '[': ++depth; break;
        case ')':
        case '}':
        case ']':
            if (depth > 0) {
                --depth;
            }
            break;
        case ';':
            if (depth == 0) {
                onSemicolon(i);
            }
            break;
        default: break;
        }
    }
    return depth > 0 || quote != 0;
}

} // namespace

ShellCommand parseStatement(std::string_view input) {
    return StatementParser(input).parse();
}

bool needsMoreInput(std::string_view buffer) {
    return scanBalance(buffer, [](std::size_t) {});
}

std::vector<std::string> splitStatements(std::string_view input) {
    std::vector<std::size_t> cuts;
    scanBalance(input, [&cuts](std::size_t i) { cuts.push_back(i); });
    std::vector<std::string> out;
    std::size_t start = 0;
    cuts.push_back(input.size());
    for (std::size_t cut : cuts) {
        std::string piece(input.substr(start, cut - start));
        std::size_t a = piece.find_first_not_of(" \t\r\n");
        if (a != std::string::npos) {
            out.push_back(piece.substr(a));
        }
        start = cut + 1;
    }
    return out;
}

} // namespace bisondb::shell
