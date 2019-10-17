#include "catch.hpp"
#include "common/file_system.hpp"
#include "test_helpers.hpp"
#include "storage/storage_info.hpp"

using namespace duckdb;
using namespace std;

TEST_CASE("Test scanning a table and computing an aggregate over a table that exceeds buffer manager size", "[storage][.]") {
	unique_ptr<MaterializedQueryResult> result;
	auto storage_database = TestCreatePath("storage_test");
	auto config = GetTestConfig();

	// set the maximum memory to 10MB
	config->maximum_memory = 10000000;

	int64_t expected_sum;
	Value sum;
	// make sure the database does not exist
	DeleteDatabase(storage_database);
	{
		// create a database and insert values
		DuckDB db(storage_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE test (a INTEGER, b INTEGER);"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO test VALUES (11, 22), (13, 22), (12, 21), (NULL, NULL)"));
		uint64_t table_size = 2 * 4 * sizeof(int);
		uint64_t desired_size = 10 * config->maximum_memory;
		expected_sum = 11 + 12 + 13 + 22 + 22 + 21;
		// grow the table until it exceeds 100MB
		while (table_size < desired_size) {
			REQUIRE_NO_FAIL(con.Query("INSERT INTO test SELECT * FROM test"));
			table_size *= 2;
			expected_sum *= 2;
		}
		sum = Value::BIGINT(expected_sum);
		// compute the sum
		result = con.Query("SELECT SUM(a) + SUM(b) FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {sum}));
	}
	{
		DuckDB db(storage_database, config.get());
		Connection con(db);
		result = con.Query("SELECT SUM(a) + SUM(b) FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {sum}));
	}
	{
		DuckDB db(storage_database, config.get());
		Connection con(db);
		result = con.Query("SELECT SUM(a) + SUM(b) FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {sum}));
	}
	DeleteDatabase(storage_database);
}

TEST_CASE("Test storing a big string that exceeds buffer manager size", "[storage][.]") {
	unique_ptr<MaterializedQueryResult> result;
	auto storage_database = TestCreatePath("storage_test");
	auto config = GetTestConfig();

	uint64_t string_length = 64;
	uint64_t desired_size = 10000000; // desired size is 10MB
	uint64_t iteration = 2;
	// make sure the database does not exist
	DeleteDatabase(storage_database);
	{
		// create a database and insert the big string
		DuckDB db(storage_database, config.get());
		Connection con(db);
		string big_string = string(string_length, 'a');
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE test (a VARCHAR, j BIGINT);"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO test VALUES ('" + big_string + "', 1)"));
		while (string_length < desired_size) {
			REQUIRE_NO_FAIL(con.Query("INSERT INTO test SELECT a||a||a||a||a||a||a||a||a||a, " + to_string(iteration) +
			                          " FROM test"));
			REQUIRE_NO_FAIL(con.Query("DELETE FROM test WHERE j=" + to_string(iteration - 1)));
			iteration++;
			string_length *= 10;
		}

		// check the length
		result = con.Query("SELECT LENGTH(a) FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(string_length)}));
		result = con.Query("SELECT j FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(iteration - 1)}));

	}
	{
		DuckDB db(storage_database, config.get());
		Connection con(db);
		result = con.Query("SELECT LENGTH(a) FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(string_length)}));
		result = con.Query("SELECT j FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(iteration - 1)}));
	}
	// now reload the database, but this time with a max memory of 5MB
	{
		config->maximum_memory = 5000000;
		DuckDB db(storage_database, config.get());
		Connection con(db);
		// we can still select the integer
		result = con.Query("SELECT j FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(iteration - 1)}));
		// however the string is too big to fit in our buffer manager
		REQUIRE_FAIL(con.Query("SELECT LENGTH(a) FROM test"));
	}
	{
		// reloading with a bigger limit again makes it work
		config->maximum_memory = (index_t) -1;
		DuckDB db(storage_database, config.get());
		Connection con(db);
		result = con.Query("SELECT LENGTH(a) FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(string_length)}));
		result = con.Query("SELECT j FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(iteration - 1)}));
	}
	DeleteDatabase(storage_database);
}

TEST_CASE("Test appending and checkpointing a table that exceeds buffer manager size", "[storage][.]") {
	unique_ptr<MaterializedQueryResult> result;
	auto storage_database = TestCreatePath("storage_test");
	auto config = GetTestConfig();

	// maximum memory is 10MB
	config->maximum_memory = 10000000;

	// create a table of size 10 times the buffer pool size
	uint64_t size = 0, size_a, sum_a, sum_b;
	uint64_t table_size = 100000000 / sizeof(int32_t);
	// make sure the database does not exist
	DeleteDatabase(storage_database);
	{
		// create a database and insert the big string
		DuckDB db(storage_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE test (a INTEGER, b INTEGER);"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO test VALUES (1, 10), (2, 20), (3, 30), (NULL, NULL)"));
		size_a = 3;
		sum_a = 1 + 2 + 3;
		sum_b = 10 + 20 + 30;
		for(size = 4; size < table_size; size *= 2) {
			REQUIRE_NO_FAIL(con.Query("INSERT INTO test SELECT * FROM test"));
			size_a *= 2;
			sum_a *= 2;
			sum_b *= 2;
		}

		// check the aggregate statistics of the table
		result = con.Query("SELECT COUNT(*), COUNT(a), SUM(a), SUM(b) FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(size)}));
		REQUIRE(CHECK_COLUMN(result, 1, {Value::BIGINT(size_a)}));
		REQUIRE(CHECK_COLUMN(result, 2, {Value::BIGINT(sum_a)}));
		REQUIRE(CHECK_COLUMN(result, 3, {Value::BIGINT(sum_b)}));

	}
	{
		// reload the table and checkpoint, still with a 10MB limit
		DuckDB db(storage_database, config.get());
		Connection con(db);

		result = con.Query("SELECT COUNT(*), COUNT(a), SUM(a), SUM(b) FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(size)}));
		REQUIRE(CHECK_COLUMN(result, 1, {Value::BIGINT(size_a)}));
		REQUIRE(CHECK_COLUMN(result, 2, {Value::BIGINT(sum_a)}));
		REQUIRE(CHECK_COLUMN(result, 3, {Value::BIGINT(sum_b)}));
	}
	DeleteDatabase(storage_database);
}