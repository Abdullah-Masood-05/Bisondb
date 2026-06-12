#pragma once

#include "core/error.hpp"
#include "core/value.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bisondb::shell {

// Carries the byte offset into the statement for caret diagnostics.
class ShellParseError : public Error {
  public:
    ShellParseError(std::size_t position, const std::string& message)
        : Error(message), position_(position) {}
    std::size_t position() const noexcept { return position_; }

  private:
    std::size_t position_;
};

enum class Verb {
    InsertOne,
    InsertMany,
    Find,
    Count,
    DeleteMany,
    UpdateOne,
    CreateIndex,
    DropIndex,
    GetIndexes,
    Drop,
    Compact,
    ShowCollections,
    ShowStatus,
    Help,
    Exit,
};

struct ShellCommand {
    Verb verb;
    std::string coll;        // db.<coll>.* statements
    std::string field;       // createIndex / dropIndex
    std::vector<Value> args; // JSON arguments (filter, doc(s), update)
    std::optional<int64_t> limit;
    std::optional<int64_t> skip;
    bool explain = false;
};

// Parses one complete statement. JSON arguments use relaxed parsing
// (unquoted keys, single quotes, trailing commas). Throws ShellParseError.
ShellCommand parseStatement(std::string_view input);

// True when the buffer has unbalanced (){}[] or an unterminated string —
// the REPL keeps reading continuation lines until this returns false.
bool needsMoreInput(std::string_view buffer);

// Splits "--eval" input into statements at top-level ';' (never inside
// strings or brackets). Empty pieces are dropped.
std::vector<std::string> splitStatements(std::string_view input);

} // namespace bisondb::shell
