#include "catch.hpp"
#include "duckdb.hpp"
#include "core_functions_extension.hpp"
#include "from_substrait.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/main/relation.hpp"
#include "duckdb/main/query_result.hpp"

using namespace duckdb;

namespace {
class ImportHandler final : public SubstraitExtensionHandler {
public:
	mutable unsigned types = 0, literals = 0, scalars = 0, aggregates = 0;
	bool Handles(const SubstraitExtensionIdentity &id) const override {
		return id.urn == "urn:test:exact:v1";
	}
	LogicalType Type(ClientContext &, const SubstraitExtensionIdentity &id,
	                 const substrait::Type &type) const override {
		REQUIRE(id.name == "exact");
		REQUIRE(type.user_defined().type_parameters_size() == 3);
		REQUIRE(type.user_defined().type_parameters(0).integer() == 256);
		REQUIRE(type.user_defined().type_parameters(1).integer() == 65);
		REQUIRE(type.user_defined().type_parameters(2).integer() == 4);
		types++;
		return LogicalType::BIGINT; // test-only carrier, not numeric execution
	}
	unique_ptr<ParsedExpression> Literal(ClientContext &, const SubstraitExtensionIdentity &id,
	                                     const substrait::Expression_Literal &literal) const override {
		REQUIRE(id.name == "exact");
		REQUIRE(literal.user_defined().type_parameters_size() == 3);
		REQUIRE(literal.user_defined().value().type_url() == "urn:test:coefficient");
		REQUIRE(literal.user_defined().value().value() == "coefficient");
		literals++;
		return make_uniq<ConstantExpression>(Value::BIGINT(23));
	}
	unique_ptr<ParsedExpression> Scalar(ClientContext &, const SubstraitExtensionIdentity &id,
	                                    const substrait::Expression_ScalarFunction &fn,
	                                    vector<unique_ptr<ParsedExpression>> children) const override {
		REQUIRE(id.name == "identity:complete-signature");
		REQUIRE(fn.has_output_type());
		REQUIRE(children.size() == 1);
		scalars++;
		return std::move(children[0]);
	}
	unique_ptr<ParsedExpression> Aggregate(ClientContext &, const SubstraitExtensionIdentity &id,
	                                       const substrait::AggregateFunction &fn,
	                                       vector<unique_ptr<ParsedExpression>> children) const override {
		REQUIRE(id.name == "total:complete-signature");
		REQUIRE(fn.has_output_type());
		REQUIRE(children.size() == 1);
		aggregates++;
		return make_uniq<FunctionExpression>("sum", std::move(children));
	}
};

substrait::Plan BasePlan() {
	substrait::Plan plan;
	auto *urn = plan.add_extension_urns();
	urn->set_extension_urn_anchor(1);
	urn->set_urn("urn:test:exact:v1");
	auto *type = plan.add_extensions()->mutable_extension_type();
	type->set_type_anchor(11);
	type->set_extension_urn_reference(1);
	type->set_name("exact");
	for (auto entry :
	     {std::make_pair(21, "identity:complete-signature"), std::make_pair(22, "total:complete-signature")}) {
		auto *function = plan.add_extensions()->mutable_extension_function();
		function->set_function_anchor(entry.first);
		function->set_extension_urn_reference(1);
		function->set_name(entry.second);
	}
	plan.add_relations()->mutable_root()->add_names("r");
	return plan;
}

void ReadOne(substrait::Rel *rel) {
	auto *read = rel->mutable_read();
	read->mutable_base_schema()->add_names("seed");
	read->mutable_base_schema()->mutable_struct_()->add_types()->mutable_i64()->set_nullability(
	    substrait::Type::NULLABILITY_REQUIRED);
	read->mutable_virtual_table()->add_expressions()->add_fields()->mutable_literal()->set_i64(1);
}

template <class Parameterized>
void Parameters(Parameterized *type) {
	for (int value : {256, 65, 4}) {
		type->add_type_parameters()->set_integer(value);
	}
}

unique_ptr<QueryResult> Execute(Connection &con, const substrait::Plan &plan,
                                shared_ptr<SubstraitExtensionHandler> handler) {
	SubstraitToDuckDB converter(con.context, plan.SerializeAsString(), false, false, std::move(handler));
	return converter.TransformPlan()->Execute();
}

void CheckValue(unique_ptr<QueryResult> result, int64_t expected) {
	REQUIRE_FALSE(result->HasError());
	auto chunk = result->Fetch();
	REQUIRE(chunk);
	REQUIRE(chunk->size() == 1);
	REQUIRE(chunk->GetValue(0, 0).GetValue<int64_t>() == expected);
}
} // namespace

TEST_CASE("Query-scoped extension import preserves identities and parameters", "[substrait-extension-import]") {
	DuckDB db(nullptr);
	db.LoadStaticExtension<CoreFunctionsExtension>();
	Connection con(db);
	auto handler = make_shared_ptr<ImportHandler>();
	SECTION("type and typed NULL") {
		auto plan = BasePlan();
		auto *project = plan.mutable_relations(0)->mutable_root()->mutable_input()->mutable_project();
		ReadOne(project->mutable_input());
		project->mutable_common()->mutable_emit()->add_output_mapping(1);
		auto *cast = project->add_expressions()->mutable_cast();
		cast->mutable_input()->mutable_literal()->set_i64(7);
		auto *type = cast->mutable_type()->mutable_user_defined();
		type->set_type_reference(11);
		Parameters(type);
		CheckValue(Execute(con, plan, handler), 7);
		REQUIRE(handler->types == 1);
		REQUIRE_THROWS(Execute(con, plan, nullptr));
		auto null_type = cast->type();
		auto *literal = project->mutable_expressions(0)->mutable_literal();
		literal->mutable_null()->CopyFrom(null_type);
		auto result = Execute(con, plan, handler);
		REQUIRE_FALSE(result->HasError());
		REQUIRE(result->Fetch()->GetValue(0, 0).IsNull());
	}
	SECTION("literal") {
		auto plan = BasePlan();
		auto *project = plan.mutable_relations(0)->mutable_root()->mutable_input()->mutable_project();
		ReadOne(project->mutable_input());
		project->mutable_common()->mutable_emit()->add_output_mapping(1);
		auto *literal = project->add_expressions()->mutable_literal()->mutable_user_defined();
		literal->set_type_reference(11);
		Parameters(literal);
		literal->mutable_value()->set_type_url("urn:test:coefficient");
		literal->mutable_value()->set_value("coefficient");
		CheckValue(Execute(con, plan, handler), 23);
		REQUIRE(handler->literals == 1);
		REQUIRE_THROWS(Execute(con, plan, nullptr));
	}
	SECTION("scalar") {
		auto plan = BasePlan();
		auto *project = plan.mutable_relations(0)->mutable_root()->mutable_input()->mutable_project();
		ReadOne(project->mutable_input());
		project->mutable_common()->mutable_emit()->add_output_mapping(1);
		auto *fn = project->add_expressions()->mutable_scalar_function();
		fn->set_function_reference(21);
		fn->mutable_output_type()->mutable_i64();
		fn->add_arguments()->mutable_value()->mutable_literal()->set_i64(9);
		CheckValue(Execute(con, plan, handler), 9);
		REQUIRE(handler->scalars == 1);
	}
	SECTION("aggregate") {
		auto plan = BasePlan();
		auto *aggregate = plan.mutable_relations(0)->mutable_root()->mutable_input()->mutable_aggregate();
		ReadOne(aggregate->mutable_input());
		auto *fn = aggregate->add_measures()->mutable_measure();
		fn->set_function_reference(22);
		fn->mutable_output_type()->mutable_i64();
		fn->add_arguments()
		    ->mutable_value()
		    ->mutable_selection()
		    ->mutable_direct_reference()
		    ->mutable_struct_field()
		    ->set_field(0);
		CheckValue(Execute(con, plan, handler), 1);
		REQUIRE(handler->aggregates == 1);
	}
	SECTION("duplicate and unknown anchors") {
		auto plan = BasePlan();
		plan.add_extension_urns()->CopyFrom(plan.extension_urns(0));
		REQUIRE_THROWS_AS(SubstraitToDuckDB(con.context, plan.SerializeAsString(), false, false, handler),
		                  InvalidInputException);
		plan = BasePlan();
		plan.mutable_extensions(0)->mutable_extension_type()->set_extension_urn_reference(99);
		REQUIRE_THROWS_AS(SubstraitToDuckDB(con.context, plan.SerializeAsString(), false, false, handler),
		                  InvalidInputException);
	}
	SECTION("ordinary imports remain independent") {
		auto plan = BasePlan();
		ReadOne(plan.mutable_relations(0)->mutable_root()->mutable_input());
		CheckValue(Execute(con, plan, nullptr), 1);
		CheckValue(Execute(con, plan, handler), 1);
		REQUIRE(handler->types == 0);
		REQUIRE(handler->literals == 0);
		REQUIRE(handler->scalars == 0);
		REQUIRE(handler->aggregates == 0);
	}
	SECTION("declared but unhandled user type is rejected") {
		auto plan = BasePlan();
		plan.mutable_extension_urns(0)->set_urn("urn:other:extension");
		auto *project = plan.mutable_relations(0)->mutable_root()->mutable_input()->mutable_project();
		ReadOne(project->mutable_input());
		project->mutable_common()->mutable_emit()->add_output_mapping(1);
		auto *cast = project->add_expressions()->mutable_cast();
		cast->mutable_input()->mutable_literal()->set_i64(7);
		cast->mutable_type()->mutable_user_defined()->set_type_reference(11);
		REQUIRE_THROWS_AS(Execute(con, plan, handler), NotImplementedException);
		REQUIRE(handler->types == 0);
	}
}
