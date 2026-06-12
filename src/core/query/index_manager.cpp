#include "core/query/index_manager.hpp"

#include "core/bson_encoder.hpp"
#include "core/btree/key_codec.hpp"
#include "core/json_parser.hpp"
#include "core/json_writer.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace bisondb::query {

namespace {

namespace fs = std::filesystem;

// Opens a B+Tree, deleting and recreating the file when it was not closed
// cleanly. Returns whether the result is freshly created (and so needs a
// rebuild from the log).
std::pair<std::unique_ptr<btree::BTree>, bool> openTreeOrRecreate(const std::string& path) {
    bool fresh = !fs::exists(path);
    try {
        auto tree = std::make_unique<btree::BTree>(path);
        return {std::move(tree), fresh};
    } catch (const btree::RebuildRequired&) {
        fs::remove(path);
        return {std::make_unique<btree::BTree>(path), true};
    }
}

} // namespace

IndexedCollection::IndexedCollection(const std::string& dbdir, const std::string& name)
    : dbdir_(dbdir), name_(name), log_(dbdir, name) {
    auto [idTree, idFresh] = openTreeOrRecreate(indexPath("_id"));
    idIndex_ = std::move(idTree);
    if (idFresh) {
        rebuildIdIndex();
    }
    loadMeta();
    for (auto& [field, tree] : fieldIndexes_) {
        auto [opened, fresh] = openTreeOrRecreate(indexPath(field));
        tree = std::move(opened);
        if (fresh) {
            indexStats_[field] = buildFieldIndex(*tree, field);
        }
    }
}

std::string IndexedCollection::indexPath(const std::string& field) const {
    return (fs::path(dbdir_) / (name_ + "." + field + ".idx")).string();
}

std::vector<uint8_t> IndexedCollection::offsetValue(uint64_t offset) {
    std::vector<uint8_t> v(8);
    for (int i = 0; i < 8; ++i) {
        v[static_cast<std::size_t>(i)] = static_cast<uint8_t>(offset >> (8 * i));
    }
    return v;
}

uint64_t IndexedCollection::offsetFromValue(std::span<const uint8_t> v) {
    uint64_t out = 0;
    for (int i = 7; i >= 0; --i) {
        out = (out << 8) | v[static_cast<std::size_t>(i)];
    }
    return out;
}

void IndexedCollection::rebuildIdIndex() {
    // Phase 2 fallback: full log replay builds the oid -> offset map, which
    // then populates a fresh _id tree.
    auto map = log_.buildOffsetMap();
    for (const auto& [oidBytes, offset] : map) {
        ObjectId oid;
        std::memcpy(oid.bytes.data(), oidBytes.data(), 12);
        auto key = btree::encodeKey(Value(oid));
        auto val = offsetValue(offset);
        idIndex_->insert({key.data(), key.size()}, {val.data(), val.size()});
    }
    idIndex_->sync();
}

IndexBuildStats IndexedCollection::buildFieldIndex(btree::BTree& tree, const std::string& field) {
    IndexBuildStats stats;
    for (auto c = idIndex_->lowerBound({}); c.valid(); c.next()) {
        uint64_t offset = offsetFromValue(c.value());
        Value doc = log_.readDocumentAt(offset);
        ObjectId oid = store::requireDocId(doc);
        const Value* fieldVal = store::lookupPath(doc, field);
        if (fieldVal == nullptr) {
            ++stats.skipped;
            continue;
        }
        try {
            auto encoded = btree::encodeKey(*fieldVal);
            auto composite = btree::composeIndexKey(encoded, oid);
            tree.insert({composite.data(), composite.size()}, {}, /*allowDuplicates=*/true);
            ++stats.indexed;
        } catch (const btree::KeyNotIndexable&) {
            ++stats.skipped;
        } catch (const btree::KeyTooLong&) {
            ++stats.skipped;
        }
    }
    tree.sync();
    return stats;
}

void IndexedCollection::loadMeta() {
    fs::path metaPath = fs::path(dbdir_) / (name_ + ".meta.json");
    if (!fs::exists(metaPath)) {
        return;
    }
    std::ifstream in(metaPath);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Value meta = parseJson(text);
    if (const Value* indexes = meta.asDocument().find("indexes")) {
        for (const Value& f : indexes->asArray()) {
            fieldIndexes_[f.get<std::string>()] = nullptr; // opened by the caller
        }
    }
}

void IndexedCollection::saveMeta() {
    Array fields;
    for (const auto& [field, tree] : fieldIndexes_) {
        fields.push_back(Value(field));
    }
    Value meta(Document{{"indexes", Value(std::move(fields))}});
    fs::path metaPath = fs::path(dbdir_) / (name_ + ".meta.json");
    std::ofstream out(metaPath, std::ios::trunc);
    out << toJson(meta, JsonMode::Relaxed, /*pretty=*/true) << "\n";
}

void IndexedCollection::indexDocument(const ObjectId& oid, const Value& doc, bool add) {
    for (auto& [field, tree] : fieldIndexes_) {
        const Value* fieldVal = store::lookupPath(doc, field);
        if (fieldVal == nullptr) {
            continue;
        }
        try {
            auto encoded = btree::encodeKey(*fieldVal);
            auto composite = btree::composeIndexKey(encoded, oid);
            if (add) {
                tree->insert({composite.data(), composite.size()}, {},
                             /*allowDuplicates=*/true);
            } else {
                tree->erase({composite.data(), composite.size()});
            }
        } catch (const btree::KeyNotIndexable&) {
            // Skipped on insert, so nothing to remove either.
        } catch (const btree::KeyTooLong&) {
        }
    }
}

ObjectId IndexedCollection::insert(Value doc) {
    if (!doc.is<Document>()) {
        throw store::StoreError("insert requires a Document value");
    }
    std::unique_lock lock(mutex_);
    Document& d = doc.asDocument();
    ObjectId oid;
    if (const Value* id = d.find("_id")) {
        if (!id->is<ObjectId>()) {
            throw store::StoreError("_id must be an ObjectId");
        }
        oid = id->get<ObjectId>();
    } else {
        oid = store::generateObjectId();
        // _id leads the document, like MongoDB writes it.
        Document withId{{"_id", Value(oid)}};
        for (auto& [k, v] : d) {
            withId.append(k, std::move(v));
        }
        doc = Value(std::move(withId));
    }

    auto idKey = btree::encodeKey(Value(oid));
    if (idIndex_->get({idKey.data(), idKey.size()}).has_value()) {
        throw btree::DuplicateKeyError("duplicate _id: " + oid.toHex());
    }
    uint64_t offset = log_.appendPut(encodeDocument(doc));
    auto val = offsetValue(offset);
    idIndex_->insert({idKey.data(), idKey.size()}, {val.data(), val.size()});
    indexDocument(oid, doc, /*add=*/true);
    return oid;
}

std::optional<Value> IndexedCollection::fetch(const ObjectId& oid) {
    std::shared_lock lock(mutex_);
    auto idKey = btree::encodeKey(Value(oid));
    auto val = idIndex_->get({idKey.data(), idKey.size()});
    if (!val) {
        return std::nullopt;
    }
    return log_.readDocumentAt(offsetFromValue(*val));
}

bool IndexedCollection::eraseById(const ObjectId& oid) {
    std::unique_lock lock(mutex_);
    auto idKey = btree::encodeKey(Value(oid));
    auto val = idIndex_->get({idKey.data(), idKey.size()});
    if (!val) {
        return false;
    }
    Value doc = log_.readDocumentAt(offsetFromValue(*val));
    log_.appendDelete(oid);
    idIndex_->erase({idKey.data(), idKey.size()});
    indexDocument(oid, doc, /*add=*/false);
    return true;
}

void IndexedCollection::update(const ObjectId& oid, Value newDoc) {
    std::unique_lock lock(mutex_);
    auto idKey = btree::encodeKey(Value(oid));
    auto val = idIndex_->get({idKey.data(), idKey.size()});
    if (!val) {
        throw store::StoreError("update: no document with _id " + oid.toHex());
    }
    if (store::requireDocId(newDoc) != oid) {
        throw store::StoreError("update may not change _id");
    }
    Value oldDoc = log_.readDocumentAt(offsetFromValue(*val));
    uint64_t offset = log_.appendPut(encodeDocument(newDoc));
    idIndex_->erase({idKey.data(), idKey.size()});
    auto newVal = offsetValue(offset);
    idIndex_->insert({idKey.data(), idKey.size()}, {newVal.data(), newVal.size()});
    indexDocument(oid, oldDoc, /*add=*/false);
    indexDocument(oid, newDoc, /*add=*/true);
}

IndexBuildStats IndexedCollection::createIndex(const std::string& field) {
    std::unique_lock lock(mutex_);
    if (field == "_id" || fieldIndexes_.contains(field)) {
        return indexStats_.contains(field) ? indexStats_[field] : IndexBuildStats{};
    }
    fs::remove(indexPath(field));
    auto tree = std::make_unique<btree::BTree>(indexPath(field));
    IndexBuildStats stats = buildFieldIndex(*tree, field);
    fieldIndexes_[field] = std::move(tree);
    indexStats_[field] = stats;
    saveMeta();
    return stats;
}

bool IndexedCollection::dropIndex(const std::string& field) {
    std::unique_lock lock(mutex_);
    auto it = fieldIndexes_.find(field);
    if (it == fieldIndexes_.end()) {
        return false;
    }
    fieldIndexes_.erase(it);
    indexStats_.erase(field);
    fs::remove(indexPath(field));
    saveMeta();
    return true;
}

std::vector<std::string> IndexedCollection::listIndexes() const {
    std::vector<std::string> out{"_id"};
    for (const auto& [field, tree] : fieldIndexes_) {
        out.push_back(field);
    }
    return out;
}

bool IndexedCollection::hasIndex(const std::string& field) const {
    return fieldIndexes_.contains(field);
}

btree::BTree* IndexedCollection::fieldIndex(const std::string& field) {
    auto it = fieldIndexes_.find(field);
    return it == fieldIndexes_.end() ? nullptr : it->second.get();
}

std::size_t IndexedCollection::count() {
    std::shared_lock lock(mutex_);
    std::size_t n = 0;
    for (auto c = idIndex_->lowerBound({}); c.valid(); c.next()) {
        ++n;
    }
    return n;
}

void IndexedCollection::compact() {
    std::unique_lock lock(mutex_);
    std::vector<uint64_t> liveOffsets;
    for (auto c = idIndex_->lowerBound({}); c.valid(); c.next()) {
        liveOffsets.push_back(offsetFromValue(c.value()));
    }
    log_.compact(liveOffsets);

    // Offsets moved: rebuild every index from the compacted log.
    idIndex_.reset();
    fs::remove(indexPath("_id"));
    idIndex_ = std::make_unique<btree::BTree>(indexPath("_id"));
    rebuildIdIndex();
    for (auto& [field, tree] : fieldIndexes_) {
        tree.reset();
        fs::remove(indexPath(field));
        tree = std::make_unique<btree::BTree>(indexPath(field));
        indexStats_[field] = buildFieldIndex(*tree, field);
    }
}

void IndexedCollection::sync() {
    std::unique_lock lock(mutex_);
    log_.sync();
    idIndex_->sync();
    for (auto& [field, tree] : fieldIndexes_) {
        tree->sync();
    }
}

} // namespace bisondb::query
