#include "duckdb/common/error_data.hpp"
#include "yaml_reader.hpp"
#include "yaml_utils.hpp"
#include "json_structure.hpp"
#include "json_scan.hpp"
#include "json_transform.hpp"
#include "duckdb/storage/arena_allocator.hpp"
#include "yyjson_memory.hpp"

namespace duckdb {

using YAMLJSONDocument = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>;

static YAMLJSONDocument ReadJSONNode(const YAML::Node &node, JSONAllocator &allocator, yyjson_read_flag flags,
                                     bool inference = false) {
	auto json = yaml_utils::YAMLNodeToJSON(node, inference);
	return YAMLJSONDocument(JSONCommon::ReadDocument(json.data(), json.size(), flags, allocator.GetYYAlc()),
	                        yyjson_doc_free);
}

LogicalType YAMLReader::DetectYAMLType(ClientContext &context, const YAML::Node &node) {
	return DetectJaggedYAMLType(context, {node});
}

static unique_ptr<JSONStructureNode> DetectYAMLStructure(ClientContext &context, const vector<YAML::Node> &nodes,
                                                         const vector<string> &sources) {
	JSONReaderOptions options;
	auto structure = make_uniq<JSONStructureNode>();
	ArenaAllocator allocator(Allocator::Get(context));
	Vector strings(LogicalType::VARCHAR);
	JSONScanData scan;
	scan.InitializeFormats(true);
	MutableDateFormatMap mutable_formats(*scan.date_format_map);
	for (idx_t i = 0; i < nodes.size(); i++) {
		const auto &node = nodes[i];
		try {
			JSONAllocator json_allocator(Allocator::Get(context));
			auto doc = ReadJSONNode(node, json_allocator, JSONCommon::READ_FLAG, true);
			auto value = yyjson_doc_get_root(doc.get());
			JSONStructure::ExtractStructure(value, *structure, false, false);
			if (structure->ContainsVarchar()) {
				structure->InitializeCandidateTypes(options.max_depth, options.convert_strings_to_integers);
				structure->RefineCandidateTypes(&value, 1, strings, allocator, mutable_formats);
			}
		} catch (const OutOfMemoryException &) {
			throw;
		} catch (const std::exception &e) {
			throw IOException("YAML source '%s', line %d, column %d: %s", i < sources.size() ? sources[i] : "<input>",
			                  node.Mark().line + 1, node.Mark().column + 1, ErrorData(e).RawMessage());
		}
		allocator.Reset();
	}
	return structure;
}

LogicalType YAMLReader::DetectJaggedYAMLType(ClientContext &context, const vector<YAML::Node> &nodes,
                                             const vector<string> &sources) {
	JSONReaderOptions options;
	auto structure = DetectYAMLStructure(context, nodes, sources);
	return JSONStructure::StructureToType(context, *structure, options.max_depth, options.field_appearance_threshold,
	                                      options.map_inference_threshold);
}

bool YAMLReader::DetectYAMLColumns(ClientContext &context, const vector<YAML::Node> &nodes, vector<LogicalType> &types,
                                   vector<Identifier> &names, const vector<string> &sources) {
	JSONReaderOptions options;
	auto structure = DetectYAMLStructure(context, nodes, sources);
	vector<JSONFeatureColumn> features;
	JSONScan::StructureToColumns(context, options, *structure, features, types, names);
	return options.record_type == JSONRecordType::VALUES;
}

Value YAMLReader::YAMLNodeToValue(const YAML::Node &node, const LogicalType &target_type, bool ignore_errors) {
	if (target_type.HasAlias() && target_type.GetAlias() == "yaml") {
		return Value(yaml_utils::EmitYAML(node, yaml_utils::YAMLFormat::FLOW));
	}
	try {
		JSONAllocator allocator(Allocator::DefaultAllocator());
		auto doc = ReadJSONNode(node, allocator, YYJSON_READ_NUMBER_AS_RAW);
		auto value = yyjson_doc_get_root(doc.get());
		Vector result(target_type, 1);
		JSONTransformOptions options(!ignore_errors, true, false, false);
		JSONScanData scan;
		scan.InitializeFormats(true);
		options.date_format_map = scan.date_format_map.get();
		if (!JSONTransform::Transform(&value, allocator.GetYYAlc(), result, 1, options, nullptr)) {
			throw InvalidInputException("%s", options.error_message);
		}
		return result.GetValue(0);
	} catch (const OutOfMemoryException &) {
		throw;
	} catch (const std::exception &e) {
		throw;
	}
}

} // namespace duckdb
