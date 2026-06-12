#include "shell/parser.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <string>

using namespace bisondb;
using namespace bisondb::shell;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("every statement form parses", "[shell_parser]") {
    SECTION("insertOne / insertMany") {
        ShellCommand c = parseStatement("db.students.insertOne({name: 'ada', cgpa: 3.9})");
        REQUIRE(c.verb == Verb::InsertOne);
        REQUIRE(c.coll == "students");
        REQUIRE(c.args[0].asDocument().find("name")->get<std::string>() == "ada");

        c = parseStatement("db.s.insertMany([{a: 1}, {a: 2},])");
        REQUIRE(c.verb == Verb::InsertMany);
        REQUIRE(c.args[0].asArray().size() == 2);
    }
    SECTION("find with and without filter") {
        REQUIRE(parseStatement("db.s.find()").args[0] == Value(Document{}));
        ShellCommand c = parseStatement("db.s.find({cgpa: {$gt: 3.5}})");
        REQUIRE(c.verb == Verb::Find);
        REQUIRE(c.args[0].asDocument().find("cgpa") != nullptr);
    }
    SECTION("count / deleteMany / updateOne") {
        REQUIRE(parseStatement("db.s.count()").verb == Verb::Count);
        REQUIRE(parseStatement("db.s.count({a: 1})").verb == Verb::Count);
        REQUIRE(parseStatement("db.s.deleteMany({})").verb == Verb::DeleteMany);
        ShellCommand c = parseStatement("db.s.updateOne({a: 1}, {$set: {b: 2}})");
        REQUIRE(c.verb == Verb::UpdateOne);
        REQUIRE(c.args.size() == 2);
    }
    SECTION("createIndex accepts a string or {field: 1}") {
        REQUIRE(parseStatement(R"(db.s.createIndex("cgpa"))").field == "cgpa");
        REQUIRE(parseStatement("db.s.createIndex({cgpa: 1})").field == "cgpa");
        REQUIRE(parseStatement("db.s.dropIndex('cgpa')").field == "cgpa");
    }
    SECTION("admin verbs") {
        REQUIRE(parseStatement("db.s.getIndexes()").verb == Verb::GetIndexes);
        REQUIRE(parseStatement("db.s.drop()").verb == Verb::Drop);
        REQUIRE(parseStatement("db.s.compact()").verb == Verb::Compact);
        REQUIRE(parseStatement("show collections").verb == Verb::ShowCollections);
        REQUIRE(parseStatement("show status").verb == Verb::ShowStatus);
        REQUIRE(parseStatement("help").verb == Verb::Help);
        REQUIRE(parseStatement("exit").verb == Verb::Exit);
        REQUIRE(parseStatement("quit").verb == Verb::Exit);
    }
    SECTION("trailing semicolon is tolerated") {
        REQUIRE(parseStatement("db.s.find();").verb == Verb::Find);
    }
}

TEST_CASE("chained find modifiers in any order, each at most once", "[shell_parser]") {
    ShellCommand c = parseStatement("db.s.find({a: 1}).limit(10).skip(5)");
    REQUIRE(c.limit == 10);
    REQUIRE(c.skip == 5);
    c = parseStatement("db.s.find().skip(2).limit(3).explain()");
    REQUIRE(c.skip == 2);
    REQUIRE(c.limit == 3);
    REQUIRE(c.explain);
    c = parseStatement("db.s.find().explain().limit(1)");
    REQUIRE(c.explain);

    REQUIRE_THROWS_AS(parseStatement("db.s.find().limit(1).limit(2)"), ShellParseError);
    REQUIRE_THROWS_AS(parseStatement("db.s.find().explain().explain()"), ShellParseError);
    REQUIRE_THROWS_AS(parseStatement("db.s.find().limit(-1)"), ShellParseError);
    REQUIRE_THROWS_AS(parseStatement("db.s.find().limit('x')"), ShellParseError);
    REQUIRE_THROWS_AS(parseStatement("db.s.find().sort({a: 1})"), ShellParseError);
}

TEST_CASE("malformed inputs report the right token and position", "[shell_parser]") {
    struct Case {
        const char* input;
        const char* expectInMessage;
    };
    const Case cases[] = {
        {"db.students.find({cgpa: {$gt 3.5}})", "expected ':' after key '$gt'"},
        {"frobnicate", "statements start with"},
        {"db.", "expected a collection name"},
        {"db.../x.find()", "expected a collection name"},
        {"db.s.frobnicate()", "unknown method 'frobnicate'"},
        {"db.s.find({a: 1}) extra", "unexpected trailing input"},
        {"show tables", "expected 'collections' or 'status'"},
        {"db.s.insertMany({a: 1})", "expects an array"},
        {"db.s.updateOne({a: 1})", "',' between filter and update"},
        {"db.s.createIndex(42)", "expects \"field\" or {field: 1}"},
        {"db.s.deleteMany()", "requires a filter"},
    };
    for (const Case& c : cases) {
        INFO(c.input);
        try {
            parseStatement(c.input);
            FAIL("expected ShellParseError");
        } catch (const ShellParseError& e) {
            REQUIRE_THAT(e.what(), ContainsSubstring(c.expectInMessage));
        }
    }
}

TEST_CASE("caret position points into the statement", "[shell_parser]") {
    const std::string stmt = "db.students.find({cgpa: {$gt 3.5}})";
    try {
        parseStatement(stmt);
        FAIL("expected ShellParseError");
    } catch (const ShellParseError& e) {
        // The error position should be at/near the '3.5' after the bad key.
        REQUIRE(e.position() == stmt.find("3.5"));
    }
}

TEST_CASE("multi-line continuation detection", "[shell_parser]") {
    REQUIRE(needsMoreInput("db.s.find({a: 1"));
    REQUIRE(needsMoreInput("db.s.insertOne({"));
    REQUIRE(needsMoreInput("db.s.find(\"unterminated"));
    REQUIRE(needsMoreInput("db.s.find('unterminated"));
    REQUIRE(needsMoreInput("db.s.insertMany([{a: 1},"));
    REQUIRE_FALSE(needsMoreInput("db.s.find({a: 1})"));
    REQUIRE_FALSE(needsMoreInput("db.s.find({a: \"with ) brace\"})"));
    REQUIRE_FALSE(needsMoreInput("help"));
    // Closing brackets never go negative.
    REQUIRE_FALSE(needsMoreInput("db.s.find({a: 1}))"));
}

TEST_CASE("splitStatements honors strings and nesting", "[shell_parser]") {
    auto parts = splitStatements("db.s.insertOne({a: 'x;y'}); db.s.find(); ;");
    REQUIRE(parts.size() == 2);
    REQUIRE(parts[0] == "db.s.insertOne({a: 'x;y'})");
    REQUIRE(parts[1] == "db.s.find()");
}
