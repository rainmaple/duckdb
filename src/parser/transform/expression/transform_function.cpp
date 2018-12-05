#include "parser/expression/aggregate_expression.hpp"
#include "parser/expression/cast_expression.hpp"
#include "parser/expression/function_expression.hpp"
#include "parser/expression/operator_expression.hpp"
#include "parser/expression/star_expression.hpp"
#include "parser/transformer.hpp"

using namespace duckdb;
using namespace postgres;
using namespace std;

static bool IsAggregateFunction(const string &fun_name) {
	if (fun_name == "min" || fun_name == "max" || fun_name == "count" || fun_name == "avg" || fun_name == "sum" ||
	    fun_name == "first" || fun_name == "stddev_samp")
		return true;
	return false;
}

unique_ptr<Expression> Transformer::TransformFuncCall(FuncCall *root) {
	auto name = root->funcname;
	string schema, function_name;
	if (name->length == 2) {
		// schema + name
		schema = reinterpret_cast<value *>(name->head->data.ptr_value)->val.str;
		function_name = reinterpret_cast<value *>(name->head->next->data.ptr_value)->val.str;
	} else {
		// unqualified name
		schema = DEFAULT_SCHEMA;
		function_name = reinterpret_cast<value *>(name->head->data.ptr_value)->val.str;
	}

	auto lowercase_name = StringUtil::Lower(function_name);
	if (!IsAggregateFunction(lowercase_name)) {
		// Normal functions (i.e. built-in functions or UDFs)
		vector<unique_ptr<Expression>> children;
		if (root->args != nullptr) {
			for (auto node = root->args->head; node != nullptr; node = node->next) {
				auto child_expr = TransformExpression((Node *)node->data.ptr_value);
				children.push_back(move(child_expr));
			}
		}
		return make_unique<FunctionExpression>(schema, function_name.c_str(), children);
	} else {
		// Aggregate function
		auto agg_fun_type = StringToExpressionType("AGGREGATE_" + function_name);

		if (root->over) {
			throw NotImplementedException("Window functions (OVER/PARTITION BY)");
		}
		if (root->agg_star) {
			return make_unique<AggregateExpression>(agg_fun_type, make_unique<StarExpression>());
		} else {
			if (root->agg_distinct) {
				switch (agg_fun_type) {
				case ExpressionType::AGGREGATE_COUNT:
					agg_fun_type = ExpressionType::AGGREGATE_COUNT_DISTINCT;
					break;
				case ExpressionType::AGGREGATE_SUM:
					agg_fun_type = ExpressionType::AGGREGATE_SUM_DISTINCT;
					break;
				default:
					// makes no difference for other aggregation types
					break;
				}
			}
			if (!root->args) {
				throw NotImplementedException("Aggregation over zero columns not supported!");
			} else if (root->args->length < 2) {
				if (agg_fun_type == ExpressionType::AGGREGATE_AVG) {
					// rewrite AVG(a) to SUM(a) / COUNT(a)
					// first create the SUM
					auto sum = make_unique<AggregateExpression>(
					    root->agg_distinct ? ExpressionType::AGGREGATE_SUM_DISTINCT : ExpressionType::AGGREGATE_SUM,
					    TransformExpression((Node *)root->args->head->data.ptr_value));
					// now create the count
					auto count = make_unique<AggregateExpression>(
					    root->agg_distinct ? ExpressionType::AGGREGATE_COUNT_DISTINCT : ExpressionType::AGGREGATE_COUNT,
					    TransformExpression((Node *)root->args->head->data.ptr_value));
					// cast both to decimal
					auto sum_cast = make_unique<CastExpression>(TypeId::DECIMAL, move(sum));
					auto count_cast = make_unique<CastExpression>(TypeId::DECIMAL, move(count));
					// create the divide operator
					return make_unique<OperatorExpression>(ExpressionType::OPERATOR_DIVIDE, TypeId::DECIMAL,
					                                       move(sum_cast), move(count_cast));
				} else {
					auto child = TransformExpression((Node *)root->args->head->data.ptr_value);
					return make_unique<AggregateExpression>(agg_fun_type, move(child));
				}
			} else {
				throw NotImplementedException("Aggregation over multiple columns not supported yet...\n");
			}
		}
	}
}