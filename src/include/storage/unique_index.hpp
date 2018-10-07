//===----------------------------------------------------------------------===//
//
//                         DuckDB
//
// storage/unique_index.hpp
//
// Author: Mark Raasveldt
//
//===----------------------------------------------------------------------===//

#pragma once

#include <mutex>
#include <vector>

#include "common/types/data_chunk.hpp"
#include "common/types/tuple.hpp"

namespace duckdb {

class DataTable;
class Transaction;

struct UniqueIndexNode {
	Tuple tuple;
	size_t row_identifier;

	UniqueIndexNode *parent;
	std::unique_ptr<UniqueIndexNode> left;
	std::unique_ptr<UniqueIndexNode> right;

	UniqueIndexNode(Tuple tuple, size_t row_identifier)
	    : tuple(std::move(tuple)), row_identifier(row_identifier),
	      parent(nullptr) {}
};

//! The unique index is used to lookup whether or not multiple values have the
//! same value. It is used to efficiently enforce PRIMARY KEY, FOREIGN KEY and
//! UNIQUE constraints.
class UniqueIndex {
  public:
	UniqueIndex(DataTable &table, std::vector<TypeId> types,
	            std::vector<size_t> keys, bool allow_nulls);

	static std::string
	Append(Transaction &transaction,
	       std::vector<std::unique_ptr<UniqueIndex>> &indexes, DataChunk &chunk,
	       size_t row_identifier_start);

	// void Delete(DataChunk &chunk);
	// void Update(DataChunk &chunk);

  private:
	UniqueIndexNode *AddEntry(Transaction &transaction, Tuple tuple,
	                          size_t row_identifier);
	void RemoveEntry(UniqueIndexNode *entry);

	//! The tuple serializer
	TupleSerializer serializer;
	//! A reference to the table this Unique constraint relates to
	DataTable &table;
	//! Types of the UniqueIndex
	std::vector<TypeId> types;
	//! The set of keys that must be collectively unique
	std::vector<size_t> keys;
	//! Lock on the index
	std::mutex index_lock;
	//! Whether or not NULL values are allowed by the constraint (false for
	//! PRIMARY KEY, true for UNIQUE)
	bool allow_nulls;

	//! Root node of the index
	std::unique_ptr<UniqueIndexNode> root;
};

} // namespace duckdb