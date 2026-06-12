#include "client/client.hpp"
#include "core/bson_decoder.hpp"
#include "core/bson_encoder.hpp"
#include "core/json_parser.hpp"
#include "core/json_writer.hpp"
#include "core/platform.hpp"
#include "core/query/index_manager.hpp"
#include "core/query/query.hpp"
#include "core/version.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if defined(BISONDB_PLATFORM_WINDOWS)
    // Required so BSON written to stdout is not mangled by LF -> CRLF
    // translation. This is the only platform-specific code in the CLI.
    #include <fcntl.h>
    #include <io.h>
#endif

namespace {

using namespace bisondb;

void setStdoutBinary() {
#if defined(BISONDB_PLATFORM_WINDOWS)
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

int usage(std::ostream& os) {
    os << "bisonc " << version() << " - BSON/JSON conversion and database tool\n"
       << "\n"
       << "Usage:\n"
       << "  bisonc to-json <input.bson> [-o out.json] [--canonical] [--pretty]\n"
       << "  bisonc to-bson <input.json> [-o out.bson]\n"
       << "  bisonc inspect <input.bson>\n"
       << "\n"
       << "  bisonc db import       <dbdir> <coll> <file.bson|file.json>\n"
       << "  bisonc db find         <dbdir> <coll> '<filter-json>' [--limit N] [--explain]\n"
       << "  bisonc db delete-many  <dbdir> <coll> '<filter-json>'\n"
       << "  bisonc db create-index <dbdir> <coll> <field>\n"
       << "  bisonc db drop-index   <dbdir> <coll> <field>\n"
       << "  bisonc db indexes      <dbdir> <coll>\n"
       << "\n"
       << "Every db subcommand accepts --connect host:port to talk to a running bisond\n"
       << "instead of opening <dbdir> locally (the dbdir argument is then ignored).\n"
       << "  bisonc ping   --connect host:port\n"
       << "  bisonc status --connect host:port\n"
       << "\n"
       << "to-json reads one or more concatenated BSON documents and writes one JSON\n"
       << "document per line (JSON Lines), or indented documents with --pretty.\n"
       << "to-bson accepts a single JSON document or JSON Lines and writes\n"
       << "concatenated BSON documents. Without -o, output goes to stdout.\n";
    return os.rdbuf() == std::cout.rdbuf() ? 0 : 1;
}

std::vector<uint8_t> readFileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open input file: " + path);
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    if (in.bad()) {
        throw std::runtime_error("failed reading input file: " + path);
    }
    return data;
}

struct Options {
    std::string input;
    std::optional<std::string> output;
    bool canonical = false;
    bool pretty = false;
};

Options parseOptions(std::span<char*> args, bool allowJsonFlags) {
    Options opts;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view a = args[i];
        if (a == "-o") {
            if (i + 1 >= args.size()) {
                throw std::runtime_error("-o requires a file argument");
            }
            opts.output = args[++i];
        } else if (allowJsonFlags && a == "--canonical") {
            opts.canonical = true;
        } else if (allowJsonFlags && a == "--pretty") {
            opts.pretty = true;
        } else if (!a.empty() && a[0] == '-') {
            throw std::runtime_error("unknown option: " + std::string(a));
        } else if (opts.input.empty()) {
            opts.input = a;
        } else {
            throw std::runtime_error("unexpected argument: " + std::string(a));
        }
    }
    if (opts.input.empty()) {
        throw std::runtime_error("missing input file");
    }
    return opts;
}

// Opens the -o file, or configures and returns stdout.
std::ostream& openOutput(const Options& opts, std::ofstream& file, bool binary) {
    if (opts.output) {
        file.open(*opts.output, binary ? std::ios::binary : std::ios::out);
        if (!file) {
            throw std::runtime_error("cannot open output file: " + *opts.output);
        }
        return file;
    }
    if (binary) {
        setStdoutBinary();
    }
    return std::cout;
}

int cmdToJson(std::span<char*> args) {
    Options opts = parseOptions(args, /*allowJsonFlags=*/true);
    std::vector<uint8_t> data = readFileBytes(opts.input);
    std::ofstream file;
    std::ostream& out = openOutput(opts, file, /*binary=*/false);
    JsonMode mode = opts.canonical ? JsonMode::Canonical : JsonMode::Relaxed;

    std::span<const uint8_t> rest(data);
    while (!rest.empty()) {
        DecodeResult res = decodeOne(rest);
        out << toJson(res.document, mode, opts.pretty) << '\n';
        rest = rest.subspan(res.bytesConsumed);
    }
    out.flush();
    return 0;
}

int cmdToBson(std::span<char*> args) {
    Options opts = parseOptions(args, /*allowJsonFlags=*/false);
    std::vector<uint8_t> data = readFileBytes(opts.input);
    std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());

    std::vector<std::vector<uint8_t>> encoded;
    std::string_view rest = text;
    while (true) {
        std::size_t ws = rest.find_first_not_of(" \t\r\n");
        if (ws == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(ws);
        std::size_t consumed = 0;
        Value v = parseJsonOne(rest, consumed);
        if (!v.is<Document>()) {
            throw std::runtime_error("top-level JSON value must be an object");
        }
        encoded.push_back(encodeDocument(v));
        rest.remove_prefix(consumed);
    }
    if (encoded.empty()) {
        throw std::runtime_error("input contains no JSON documents");
    }

    std::ofstream file;
    std::ostream& out = openOutput(opts, file, /*binary=*/true);
    for (const auto& bytes : encoded) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    out.flush();
    if (!out) {
        throw std::runtime_error("failed writing output");
    }
    return 0;
}

void countValues(const Value& v, std::map<std::string_view, std::size_t>& counts) {
    counts[typeName(v.type())]++;
    if (v.is<Document>()) {
        for (const auto& [key, field] : v.asDocument()) {
            countValues(field, counts);
        }
    } else if (v.is<Array>()) {
        for (const Value& e : v.asArray()) {
            countValues(e, counts);
        }
    }
}

int cmdInspect(std::span<char*> args) {
    Options opts = parseOptions(args, /*allowJsonFlags=*/false);
    std::vector<uint8_t> data = readFileBytes(opts.input);

    std::size_t docCount = 0;
    std::map<std::string_view, std::size_t> counts;
    std::span<const uint8_t> rest(data);
    while (!rest.empty()) {
        DecodeResult res = decodeOne(rest);
        ++docCount;
        countValues(res.document, counts);
        rest = rest.subspan(res.bytesConsumed);
    }

    std::cout << "documents:   " << docCount << '\n';
    std::cout << "total bytes: " << data.size() << '\n';
    std::cout << "value counts by type:\n";
    for (const auto& [name, count] : counts) {
        std::cout << "  " << name << ": " << count << '\n';
    }
    return 0;
}

// ---- bisonc db ---------------------------------------------------------

// Extracts "--connect host:port" from the argument list (mutating `args` to
// drop the pair) and returns the endpoint when present.
std::optional<std::pair<std::string, uint16_t>> extractConnect(std::vector<char*>& args) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (std::string_view(args[i]) != "--connect") {
            continue;
        }
        if (i + 1 >= args.size()) {
            throw std::runtime_error("--connect requires host:port");
        }
        std::string endpoint = args[i + 1];
        std::size_t colon = endpoint.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= endpoint.size()) {
            throw std::runtime_error("--connect expects host:port, got " + endpoint);
        }
        std::string host = endpoint.substr(0, colon);
        uint16_t port = static_cast<uint16_t>(std::stoi(endpoint.substr(colon + 1)));
        args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
                   args.begin() + static_cast<std::ptrdiff_t>(i) + 2);
        return std::make_pair(host, port);
    }
    return std::nullopt;
}

int cmdDbRemote(client::BisonClient& c, std::string_view verb, const std::string& coll,
                std::span<char*> rest) {
    if (verb == "import") {
        if (rest.size() != 1) {
            throw std::runtime_error("db import: expected an input file");
        }
        std::string input = rest[0];
        std::vector<uint8_t> data = readFileBytes(input);
        std::vector<Value> batch;
        std::size_t imported = 0;
        auto flush = [&] {
            if (!batch.empty()) {
                imported += c.insert(coll, batch).size();
                batch.clear();
            }
        };
        if (input.size() >= 5 && input.substr(input.size() - 5) == ".json") {
            std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
            while (true) {
                std::size_t ws = text.find_first_not_of(" \t\r\n");
                if (ws == std::string_view::npos) {
                    break;
                }
                text.remove_prefix(ws);
                std::size_t consumed = 0;
                batch.push_back(parseJsonOne(text, consumed));
                text.remove_prefix(consumed);
                if (batch.size() >= 500) {
                    flush();
                }
            }
        } else {
            std::span<const uint8_t> bytes(data);
            while (!bytes.empty()) {
                DecodeResult res = decodeOne(bytes);
                batch.push_back(std::move(res.document));
                bytes = bytes.subspan(res.bytesConsumed);
                if (batch.size() >= 500) {
                    flush();
                }
            }
        }
        flush();
        std::cout << "imported " << imported << " documents\n";
        return 0;
    }
    if (verb == "find") {
        if (rest.empty()) {
            throw std::runtime_error("db find: expected a filter");
        }
        Value filter = parseJson(rest[0]);
        client::FindOptions opts;
        bool explain = false;
        for (std::size_t i = 1; i < rest.size(); ++i) {
            std::string_view a = rest[i];
            if (a == "--limit" && i + 1 < rest.size()) {
                opts.limit = static_cast<std::size_t>(std::stoull(rest[++i]));
            } else if (a == "--explain") {
                explain = true;
            } else {
                throw std::runtime_error("db find: unknown option " + std::string(a));
            }
        }
        if (explain) {
            std::cout << toJson(c.explain(coll, filter, opts.limit), JsonMode::Relaxed, true)
                      << "\n";
            return 0;
        }
        for (const Value& doc : c.find(coll, filter, opts)) {
            std::cout << toJson(doc) << "\n";
        }
        return 0;
    }
    if (verb == "delete-many") {
        if (rest.size() != 1) {
            throw std::runtime_error("db delete-many: expected a filter");
        }
        std::cout << "deleted " << c.deleteMany(coll, parseJson(rest[0])) << " documents\n";
        return 0;
    }
    if (verb == "create-index") {
        if (rest.size() != 1) {
            throw std::runtime_error("db create-index: expected a field name");
        }
        std::cout << "indexed " << c.createIndex(coll, rest[0]) << " documents\n";
        return 0;
    }
    if (verb == "drop-index") {
        if (rest.size() != 1) {
            throw std::runtime_error("db drop-index: expected a field name");
        }
        c.dropIndex(coll, rest[0]);
        std::cout << "dropped\n";
        return 0;
    }
    if (verb == "indexes") {
        for (const std::string& field : c.listIndexes(coll)) {
            std::cout << field << "\n";
        }
        return 0;
    }
    throw std::runtime_error("db: unknown verb '" + std::string(verb) + "'");
}

int cmdDb(std::span<char*> argsIn) {
    std::vector<char*> argVec(argsIn.begin(), argsIn.end());
    auto connect = extractConnect(argVec);
    std::span<char*> args(argVec);
    if (args.size() < 3) {
        throw std::runtime_error("db: expected <verb> <dbdir> <collection> ...");
    }
    std::string_view verb = args[0];
    std::string dbdir = args[1];
    std::string coll = args[2];
    std::span<char*> rest = args.subspan(3);

    if (connect) {
        client::BisonClient c = client::BisonClient::connect(connect->first, connect->second);
        return cmdDbRemote(c, verb, coll, rest);
    }

    query::IndexedCollection collection(dbdir, coll);
    query::QueryEngine engine(collection);

    if (verb == "import") {
        if (rest.size() != 1) {
            throw std::runtime_error("db import: expected an input file");
        }
        std::string input = rest[0];
        std::vector<uint8_t> data = readFileBytes(input);
        std::size_t imported = 0;
        if (input.size() >= 5 && input.substr(input.size() - 5) == ".json") {
            std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
            while (true) {
                std::size_t ws = text.find_first_not_of(" \t\r\n");
                if (ws == std::string_view::npos) {
                    break;
                }
                text.remove_prefix(ws);
                std::size_t consumed = 0;
                collection.insert(parseJsonOne(text, consumed));
                text.remove_prefix(consumed);
                ++imported;
            }
        } else {
            std::span<const uint8_t> bytes(data);
            while (!bytes.empty()) {
                DecodeResult res = decodeOne(bytes);
                collection.insert(std::move(res.document));
                bytes = bytes.subspan(res.bytesConsumed);
                ++imported;
            }
        }
        collection.sync();
        std::cout << "imported " << imported << " documents\n";
        return 0;
    }
    if (verb == "find") {
        if (rest.empty()) {
            throw std::runtime_error("db find: expected a filter");
        }
        Value filter = parseJson(rest[0]);
        query::FindOptions opts;
        bool explain = false;
        for (std::size_t i = 1; i < rest.size(); ++i) {
            std::string_view a = rest[i];
            if (a == "--limit") {
                if (i + 1 >= rest.size()) {
                    throw std::runtime_error("--limit requires a number");
                }
                opts.limit = static_cast<std::size_t>(std::stoull(rest[++i]));
            } else if (a == "--explain") {
                explain = true;
            } else {
                throw std::runtime_error("db find: unknown option " + std::string(a));
            }
        }
        if (explain) {
            std::cout << toJson(engine.explain(filter, opts).toValue(), JsonMode::Relaxed, true)
                      << "\n";
            return 0;
        }
        for (const Value& doc : engine.find(filter, opts)) {
            std::cout << toJson(doc) << "\n";
        }
        return 0;
    }
    if (verb == "delete-many") {
        if (rest.size() != 1) {
            throw std::runtime_error("db delete-many: expected a filter");
        }
        std::size_t n = engine.deleteMany(parseJson(rest[0]));
        collection.sync();
        std::cout << "deleted " << n << " documents\n";
        return 0;
    }
    if (verb == "create-index") {
        if (rest.size() != 1) {
            throw std::runtime_error("db create-index: expected a field name");
        }
        query::IndexBuildStats stats = collection.createIndex(rest[0]);
        collection.sync();
        std::cout << "indexed " << stats.indexed << " documents (" << stats.skipped
                  << " skipped)\n";
        return 0;
    }
    if (verb == "drop-index") {
        if (rest.size() != 1) {
            throw std::runtime_error("db drop-index: expected a field name");
        }
        bool dropped = collection.dropIndex(rest[0]);
        std::cout << (dropped ? "dropped\n" : "no such index\n");
        return dropped ? 0 : 2;
    }
    if (verb == "indexes") {
        for (const std::string& field : collection.listIndexes()) {
            std::cout << field << "\n";
        }
        return 0;
    }
    throw std::runtime_error("db: unknown verb '" + std::string(verb) + "'");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return usage(std::cerr);
    }
    std::string_view command = argv[1];
    std::span<char*> rest(argv + 2, static_cast<std::size_t>(argc - 2));
    try {
        if (command == "to-json") {
            return cmdToJson(rest);
        }
        if (command == "to-bson") {
            return cmdToBson(rest);
        }
        if (command == "inspect") {
            return cmdInspect(rest);
        }
        if (command == "db") {
            return cmdDb(rest);
        }
        if (command == "ping" || command == "status") {
            std::vector<char*> argVec(rest.begin(), rest.end());
            auto connect = extractConnect(argVec);
            if (!connect) {
                throw std::runtime_error(std::string(command) + " requires --connect host:port");
            }
            client::BisonClient c = client::BisonClient::connect(connect->first, connect->second);
            if (command == "ping") {
                c.ping();
                std::cout << "ok\n";
            } else {
                std::cout << toJson(c.serverStatus(), JsonMode::Relaxed, true) << "\n";
            }
            return 0;
        }
        if (command == "--help" || command == "-h" || command == "help") {
            return usage(std::cout);
        }
        std::cerr << "bisonc: unknown command '" << command << "'\n";
        return usage(std::cerr);
    } catch (const std::exception& e) {
        std::cerr << "bisonc: " << e.what() << '\n';
        return 2;
    }
}
