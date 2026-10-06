#include "yaml_utils.hpp"
#include "json_common.hpp"
#include "duckdb_compat.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/types/string_type.hpp"
#include <unordered_set>
#include <unordered_map>
#include "simdutf.h"

namespace duckdb {

namespace yaml_utils {

//===--------------------------------------------------------------------===//
// YAML Settings Implementation
//===--------------------------------------------------------------------===//

// Initialize default format to FLOW for better compatibility with SQLLogicTest
std::atomic<YAMLFormat> YAMLSettings::default_format = YAMLFormat::FLOW;

// Resource-limit defaults (see yaml_utils.hpp / GHSA-h5hw-g5m6-vmjj)
std::atomic<idx_t> YAMLSettings::max_expansion_nodes = YAML_DEFAULT_MAX_EXPANSION_NODES;
std::atomic<idx_t> YAMLSettings::max_nesting_depth = YAML_DEFAULT_MAX_NESTING_DEPTH;
std::atomic<idx_t> YAMLSettings::max_input_size = YAML_DEFAULT_MAX_INPUT_SIZE;

YAMLFormat YAMLSettings::GetDefaultFormat() {
	return default_format;
}

void YAMLSettings::SetDefaultFormat(YAMLFormat format) {
	default_format = format;
}

void YAMLSettings::SetMaxExpansionNodes(idx_t value) {
	max_expansion_nodes = value;
}
idx_t YAMLSettings::GetMaxExpansionNodes() {
	return max_expansion_nodes;
}
void YAMLSettings::SetMaxNestingDepth(idx_t value) {
	max_nesting_depth = value;
}
idx_t YAMLSettings::GetMaxNestingDepth() {
	return max_nesting_depth;
}
void YAMLSettings::SetMaxInputSize(idx_t value) {
	max_input_size = value;
}
idx_t YAMLSettings::GetMaxInputSize() {
	return max_input_size;
}

//===--------------------------------------------------------------------===//
// Traversal budget / input-size guard (GHSA-h5hw-g5m6-vmjj)
//===--------------------------------------------------------------------===//

YAMLBudgetScope::YAMLBudgetScope(YAMLTraversalBudget &budget_p) : budget(budget_p) {
	// Count this node visit first. The counter is cumulative and is never
	// decremented, so exponential re-materialization of shared alias nodes is
	// bounded regardless of depth.
	if (++budget.nodes > budget.max_nodes) {
		throw InvalidInputException(
		    "YAML expansion exceeded the maximum node budget (%llu); the input may contain an alias/anchor "
		    "expansion bomb.",
		    (unsigned long long)budget.max_nodes);
	}
	if (++budget.depth > budget.max_depth) {
		throw InvalidInputException("YAML nesting exceeded the maximum depth (%llu).",
		                            (unsigned long long)budget.max_depth);
	}
}

YAMLBudgetScope::~YAMLBudgetScope() {
	--budget.depth;
}

void CheckInputSize(idx_t size, const char *context) {
	idx_t limit = YAMLSettings::GetMaxInputSize();
	if (size > limit) {
		throw InvalidInputException("YAML input to %s (%llu bytes) exceeds the maximum allowed size (%llu bytes).",
		                            context, (unsigned long long)size, (unsigned long long)limit);
	}
}

// Estimate the expanded tree on the alias DAG before materializing any aliases.
// Marks bucket node identities so ordinary documents remain linear in their size.
struct ExpansionCost {
	YAML::Node node;
	bool active = true;
	idx_t nodes = 1;
	idx_t bytes = 1;
};

struct ExpansionCheck {
	std::unordered_map<int, vector<unique_ptr<ExpansionCost>>> seen;
	idx_t input_bytes = 0;
	YAMLTraversalBudget budget;

	ExpansionCost &Visit(const YAML::Node &node) {
		auto &bucket = seen[node.Mark().pos];
		for (auto &entry : bucket) {
			if (entry->node.is(node)) {
				if (entry->active) {
					throw YAML::ParserException(node.Mark(), "YAML cyclic alias (nesting cycle)");
				}
				return *entry;
			}
		}
		YAMLBudgetScope scope(budget);
		auto entry = make_uniq<ExpansionCost>();
		entry->node.reset(node);
		auto &cost = *entry;
		bucket.push_back(std::move(entry));
		if (node.IsScalar()) {
			const auto &scalar = node.Scalar();
			if (!simdutf::validate_utf8(scalar.data(), scalar.size())) {
				throw YAML::ParserException(node.Mark(), "Invalid UTF-8 in YAML scalar");
			}
			cost.bytes += scalar.size();
		}
		input_bytes += cost.bytes;
		auto add = [&](const YAML::Node &child) {
			auto &child_cost = Visit(child);
			cost.nodes += child_cost.nodes;
			cost.bytes += child_cost.bytes;
			if (cost.nodes > MinValue<idx_t>(budget.max_nodes, 1000000) || cost.bytes > 67108864) {
				throw YAML::ParserException(child.Mark(),
				                            "YAML expansion exceeded the maximum node budget or output size");
			}
		};
		if (node.IsMap()) {
			for (const auto &pair : node) {
				add(pair.first);
				add(pair.second);
			}
		} else if (node.IsSequence()) {
			for (const auto &child : node) {
				add(child);
			}
		}
		cost.active = false;
		return cost;
	}
};

void CheckExpansionBudget(const YAML::Node &node) {
	try {
		ExpansionCheck check;
		auto &cost = check.Visit(node);
		// Distinct node payload is a conservative lower bound on source text size.
		if (cost.bytes > MaxValue<idx_t>(65536, check.input_bytes * 64)) {
			throw YAML::ParserException(node.Mark(), "YAML alias expansion exceeds 64 times the input payload size");
		}
	} catch (const YAML::Exception &e) {
		throw InvalidInputException("%s", ErrorData(e).RawMessage());
	}
}

//===--------------------------------------------------------------------===//
// YAML Parsing and Emission
//===--------------------------------------------------------------------===//

static bool IsMergeKey(const YAML::Node &key) {
	return key.IsScalar() && key.Scalar() == "<<" && (key.Tag() == "?" || key.Tag() == "tag:yaml.org,2002:merge");
}

static YAML::Node ExpandMergesImpl(const YAML::Node &node, YAMLTraversalBudget &budget) {
	YAMLBudgetScope scope(budget);
	if (node.IsSequence()) {
		for (auto child : node) {
			YAML::Node value = child;
			value = ExpandMergesImpl(value, budget);
		}
		return node;
	}
	if (!node.IsMap()) {
		return node;
	}
	bool has_merge = false;
	std::unordered_set<string> keys;
	for (const auto &pair : node) {
		if (!pair.first.IsScalar()) {
			throw YAML::ParserException(pair.first.Mark(), pair.first.IsNull() ? "YAML mapping keys must not be null"
			                                                                   : "YAML mapping keys must be scalars");
		}
		if (!keys.insert(StringUtil::Lower(pair.first.Scalar())).second) {
			throw YAML::ParserException(pair.first.Mark(), "Duplicate key (case-insensitive): " + pair.first.Scalar());
		}
		if (IsMergeKey(pair.first)) {
			has_merge = true;
		} else {
			YAML::Node value = pair.second;
			value = ExpandMergesImpl(value, budget);
		}
	}
	if (!has_merge) {
		return node;
	}
	YAML::Node result(YAML::NodeType::Map);
	for (const auto &pair : node) {
		if (!IsMergeKey(pair.first)) {
			result.force_insert(pair.first, pair.second);
		}
	}
	for (const auto &pair : node) {
		if (!IsMergeKey(pair.first)) {
			continue;
		}
		auto source = ExpandMergesImpl(pair.second, budget);
		auto merge = [&](const YAML::Node &map) {
			if (!map.IsMap()) {
				throw YAML::ParserException(pair.first.Mark(), "YAML merge requires a mapping or sequence of mappings");
			}
			for (const auto &entry : map) {
				const auto key = entry.first.Scalar();
				if (!static_cast<const YAML::Node &>(result)[key]) {
					result.force_insert(entry.first, entry.second);
				}
			}
		};
		if (source.IsSequence()) {
			for (const auto &map : source) {
				merge(map);
			}
		} else {
			merge(source);
		}
	}
	// Preserve the source mark used by document-level diagnostics.
	YAML::Node original = node;
	original.remove("<<");
	for (const auto &entry : result) {
		if (!static_cast<const YAML::Node &>(original)[entry.first.Scalar()]) {
			original.force_insert(entry.first, entry.second);
		}
	}
	return original;
}

YAML::Node ExpandMerges(const YAML::Node &node) {
	YAMLTraversalBudget budget;
	try {
		CheckExpansionBudget(node);
		return ExpandMergesImpl(node, budget);
	} catch (const InvalidInputException &e) {
		throw YAML::ParserException(node.Mark(), ErrorData(e).RawMessage());
	}
}

std::vector<YAML::Node> ParseYAML(const std::string &yaml_str, bool multi_doc) {
	if (yaml_str.empty()) {
		return {};
	}

	try {
		std::stringstream yaml_stream(yaml_str);
		if (multi_doc) {
			auto docs = YAML::LoadAll(yaml_stream);
			for (auto &doc : docs) {
				doc.reset(ExpandMerges(doc));
			}
			return docs;
		} else {
			std::vector<YAML::Node> result;
			result.push_back(ExpandMerges(YAML::Load(yaml_stream)));
			return result;
		}
	} catch (const std::exception &e) {
		throw InvalidInputException("Error parsing YAML: %s", ErrorData(e).RawMessage());
	}
}

YAMLStringStyle ResolveStringStyle(YAMLStringStyle style, YAMLFormat format) {
	if (style != YAMLStringStyle::AUTO) {
		return style;
	}
	return (format == YAMLFormat::BLOCK) ? YAMLStringStyle::LITERAL : YAMLStringStyle::QUOTED;
}

void ConfigureEmitter(YAML::Emitter &out, YAMLFormat format, idx_t indent) {
	out.SetIndent(indent);
	if (format == YAMLFormat::FLOW) {
		out.SetMapFormat(YAML::Flow);
		out.SetSeqFormat(YAML::Flow);
	} else {
		out.SetMapFormat(YAML::Block);
		out.SetSeqFormat(YAML::Block);
	}
}

static void EmitNodeWithStringStyleImpl(YAML::Emitter &out, const YAML::Node &node, YAMLStringStyle resolved_style,
                                        YAMLTraversalBudget &budget) {
	YAMLBudgetScope scope(budget);
	switch (node.Type()) {
	case YAML::NodeType::Scalar: {
		const auto &scalar = node.Scalar();
		// Only apply Literal style to scalars that actually contain newlines
		if (resolved_style == YAMLStringStyle::LITERAL && scalar.find('\n') != std::string::npos) {
			out << YAML::Literal << scalar;
		} else if (node.Tag() == "!" || node.Tag() == "tag:yaml.org,2002:str") {
			out << YAML::DoubleQuoted << scalar;
		} else {
			out << node;
		}
		break;
	}
	case YAML::NodeType::Sequence: {
		out << YAML::BeginSeq;
		for (const auto &child : node) {
			EmitNodeWithStringStyleImpl(out, child, resolved_style, budget);
		}
		out << YAML::EndSeq;
		break;
	}
	case YAML::NodeType::Map: {
		out << YAML::BeginMap;
		for (const auto &pair : node) {
			out << YAML::Key;
			EmitNodeWithStringStyleImpl(out, pair.first, YAMLStringStyle::QUOTED, budget);
			out << YAML::Value;
			EmitNodeWithStringStyleImpl(out, pair.second, resolved_style, budget);
		}
		out << YAML::EndMap;
		break;
	}
	default:
		// Null and Undefined - delegate directly
		out << node;
		break;
	}
}

void EmitNodeWithStringStyle(YAML::Emitter &out, const YAML::Node &node, YAMLStringStyle resolved_style) {
	YAMLTraversalBudget budget;
	EmitNodeWithStringStyleImpl(out, node, resolved_style, budget);
}

std::string EmitYAML(const YAML::Node &node, YAMLFormat format, YAMLStringStyle string_style, idx_t indent) {
	if (node.IsNull()) {
		return "~";
	}
	YAML::Emitter out;
	ConfigureEmitter(out, format, indent);
	auto resolved = ResolveStringStyle(string_style, format);
	EmitNodeWithStringStyle(out, node, resolved);
	return out.c_str();
}

std::string EmitYAMLMultiDoc(const std::vector<YAML::Node> &docs, YAMLFormat format) {
	if (docs.empty()) {
		return "";
	}

	if (docs.size() == 1) {
		return EmitYAML(docs[0], format);
	}

	if (format == YAMLFormat::FLOW) {
		// For inline format, emit as sequence for readability
		YAML::Emitter out;
		ConfigureEmitter(out, format);
		out << YAML::BeginSeq;
		for (const auto &doc : docs) {
			out << doc;
		}
		out << YAML::EndSeq;
		return out.c_str();
	} else {
		// For block format, emit as multiple documents
		std::string result;
		for (idx_t doc_idx = 0; doc_idx < docs.size(); doc_idx++) {
			if (doc_idx > 0) {
				result += "\n---\n";
			}
			result += EmitYAML(docs[doc_idx], format);
		}
		return result;
	}
}

//===--------------------------------------------------------------------===//
// YAML to JSON Conversion
//===--------------------------------------------------------------------===//

// JSON-escape a string per RFC 8259 and wrap it in quotes: the named short escapes plus
// \u00XX for any control character (< 0x20). Used for BOTH string scalar values and map keys.
// Keys were previously inserted raw, so a key containing " or \ (e.g. `a"b: 1`) produced
// invalid JSON from a function declared to return LogicalType::JSON() (issue #42).
static std::string EscapeJSONString(const std::string &value) {
	std::string result = "\"";
	for (char ch : value) {
		switch (ch) {
		case '\"':
			result += "\\\"";
			break;
		case '\\':
			result += "\\\\";
			break;
		case '\b':
			result += "\\b";
			break;
		case '\f':
			result += "\\f";
			break;
		case '\n':
			result += "\\n";
			break;
		case '\r':
			result += "\\r";
			break;
		case '\t':
			result += "\\t";
			break;
		default: {
			constexpr unsigned char MIN_PRINTABLE_CHAR = 32;
			constexpr idx_t UNICODE_BUFFER_SIZE = 8;
			if (static_cast<unsigned char>(ch) < MIN_PRINTABLE_CHAR) {
				char buf[UNICODE_BUFFER_SIZE];
				snprintf(buf, sizeof(buf), "\\u%04x", ch);
				result += buf;
			} else {
				result += ch;
			}
		}
		}
	}
	result += "\"";
	return result;
}

static std::string YAMLNodeToJSONImpl(const YAML::Node &node, YAMLTraversalBudget &budget, bool inference) {
	if (!node) {
		return "null";
	}
	YAMLBudgetScope scope(budget);

	switch (node.Type()) {
	case YAML::NodeType::Null:
		return "null";
	case YAML::NodeType::Scalar: {
		const auto value = node.Scalar();

		// Quoted and explicitly tagged strings retain their JSON string representation.
		if (node.Tag() != "!" && node.Tag() != "tag:yaml.org,2002:str") {
			if (value == "true" || value == "false" || value == "null") {
				return value;
			}
			if (value.empty() || (value[0] != '-' && (value[0] < '0' || value[0] > '9'))) {
				return EscapeJSONString(value);
			}
			auto doc = yyjson_read(value.data(), value.size(), YYJSON_READ_NUMBER_AS_RAW);
			if (doc) {
				auto root = yyjson_doc_get_root(doc);
				bool scalar =
				    yyjson_is_raw(root) || yyjson_is_num(root) || yyjson_is_bool(root) || yyjson_is_null(root);
				yyjson_doc_free(doc);
				if (scalar) {
					return value;
				}
			}
		}

		// A sentinel in the inference document prevents quoted strings from matching SQL candidates.
		if (inference && (node.Tag() == "!" || node.Tag() == "tag:yaml.org,2002:str")) {
			return EscapeJSONString("yaml-string:" + value);
		}

		// If all else fails, treat as string and escape JSON special characters.
		return EscapeJSONString(value);
	}
	case YAML::NodeType::Sequence: {
		std::string result = "[";
		for (idx_t seq_idx = 0; seq_idx < node.size(); seq_idx++) {
			if (seq_idx > 0) {
				result += ",";
			}
			result += YAMLNodeToJSONImpl(node[seq_idx], budget, inference);
		}
		result += "]";
		return result;
	}
	case YAML::NodeType::Map: {
		std::string result = "{";
		bool first = true;
		for (const auto &it : node) {
			if (!first) {
				result += ",";
			}
			first = false;

			// Key must be a string in JSON — escape it (a key containing " or \ would otherwise
			// produce invalid JSON; issue #42).
			const auto key = it.first.Scalar();
			result += EscapeJSONString(key) + ":" + YAMLNodeToJSONImpl(it.second, budget, inference);
		}
		result += "}";
		return result;
	}
	default:
		throw InternalException("Unknown YAML node type");
	}
}

std::string YAMLNodeToJSON(const YAML::Node &node, bool inference) {
	YAMLTraversalBudget budget;
	return YAMLNodeToJSONImpl(node, budget, inference);
}

//===--------------------------------------------------------------------===//
// DuckDB Value to YAML Conversion
//===--------------------------------------------------------------------===//

static void EmitValueToYAMLImpl(YAML::Emitter &out, const Value &value, YAMLTraversalBudget &budget) {
	YAMLBudgetScope scope(budget);
	try {
		// Handle NULL values within data structures (emit YAML null '~')
		// Note: Top-level NULL inputs are handled at the function level following SQL semantics
		if (value.IsNull()) {
			out << YAML::Null;
			return;
		}

		switch (value.type().id()) {
		case LogicalTypeId::VARCHAR: {
			// Check if this is a JSON type (VARCHAR with JSON alias)
			if (value.type().IsJSONType()) {
				try {
					// Get the JSON string representation
					const auto json_str = value.GetValue<string>();

					// Parse JSON using YAML parser (yaml-cpp can parse JSON)
					YAML::Node json_node = YAML::Load(json_str);

					// Emit the parsed structure as YAML
					out << json_node;
				} catch (...) {
					// If JSON parsing fails, emit as quoted string
					out << YAML::SingleQuoted << value.ToString();
				}
				break;
			}

			// Handle regular VARCHAR
			const auto str_val = value.GetValue<string>();

			// Check if the string needs special formatting
			bool needs_quotes = false;

			if (str_val.empty()) {
				needs_quotes = true;
			} else {
				// Check for special strings that could be interpreted as something else
				if (str_val == "null" || str_val == "true" || str_val == "false" || str_val == "yes" ||
				    str_val == "no" || str_val == "on" || str_val == "off" || str_val == "~" || str_val == "") {
					needs_quotes = true;
				}

				// Check if it looks like a number using DuckDB's Value casting
				try {
					Value string_val(str_val);
					Value double_val = string_val.DefaultCastAs(LogicalType::DOUBLE);
					// If casting succeeded, it looks like a number
					needs_quotes = true;
				} catch (...) {
					// Not a number
				}

				// Check for special characters
				for (char ch : str_val) {
					if (ch == ':' || ch == '{' || ch == '}' || ch == '[' || ch == ']' || ch == ',' || ch == '&' ||
					    ch == '*' || ch == '#' || ch == '?' || ch == '|' || ch == '-' || ch == '<' || ch == '>' ||
					    ch == '=' || ch == '!' || ch == '%' || ch == '@' || ch == '\\' || ch == '"' || ch == '\'' ||
					    ch == '\n' || ch == '\t' || ch == ' ') {
						needs_quotes = true;
						break;
					}
				}
			}

			if (needs_quotes) {
				// Use single quoted style for most strings requiring quotes
				out << YAML::SingleQuoted << str_val;
			} else {
				// Use plain style for strings that don't need quotes
				out << str_val;
			}
			break;
		}
		case LogicalTypeId::BOOLEAN:
			out << value.GetValue<bool>();
			break;
		case LogicalTypeId::INTEGER:
		case LogicalTypeId::BIGINT:
			try {
				out << value.GetValue<int64_t>();
			} catch (...) {
				// If casting fails, convert to string as fallback
				out << YAML::SingleQuoted << value.ToString();
			}
			break;
		case LogicalTypeId::FLOAT:
		case LogicalTypeId::DOUBLE:
			try {
				out << value.GetValue<double>();
			} catch (...) {
				// If casting fails, convert to string as fallback
				out << YAML::SingleQuoted << value.ToString();
			}
			break;
		case LogicalTypeId::LIST: {
			try {
				out << YAML::BeginSeq;
				const auto &list_val = ListValue::GetChildren(value);
				for (const auto &element : list_val) {
					EmitValueToYAMLImpl(out, element, budget);
				}
				out << YAML::EndSeq;
			} catch (...) {
				// If list processing fails, emit as string
				out << YAML::SingleQuoted << value.ToString();
			}
			break;
		}
		case LogicalTypeId::STRUCT: {
			try {
				out << YAML::BeginMap;
				const auto &struct_vals = StructValue::GetChildren(value);
				const auto &struct_names = StructType::GetChildTypes(value.type());

				// Safety check for struct children
				if (struct_vals.size() != struct_names.size()) {
					throw std::runtime_error("Mismatch between struct values and names");
				}

				for (idx_t field_idx = 0; field_idx < struct_vals.size(); field_idx++) {
					out << YAML::Key << CompatIdentifierName(struct_names[field_idx].first);
					out << YAML::Value;
					EmitValueToYAMLImpl(out, struct_vals[field_idx], budget);
				}
				out << YAML::EndMap;
			} catch (...) {
				// If struct processing fails, emit as string
				out << YAML::SingleQuoted << value.ToString();
			}
			break;
		}
		default:
			// For types we don't handle specifically, convert to string
			out << YAML::SingleQuoted << value.ToString();
			break;
		}
	} catch (...) {
		// Last-resort fallback - if anything goes wrong, emit null
		try {
			out << YAML::Null;
		} catch (...) {
			// Prevent cascading exceptions
		}
	}
}

void EmitValueToYAML(YAML::Emitter &out, const Value &value) {
	YAMLTraversalBudget budget;
	EmitValueToYAMLImpl(out, value, budget);
}

// Convert DuckDB Value to YAML::Node (respects emitter configuration)
static YAML::Node ValueToYAMLNodeImpl(const Value &value, YAMLTraversalBudget &budget) {
	YAMLBudgetScope scope(budget);
	if (value.IsNull()) {
		return YAML::Node(YAML::NodeType::Null);
	}

	switch (value.type().id()) {
	case LogicalTypeId::VARCHAR: {
		if (value.type().IsJSONType()) {
			try {
				std::string json_str = value.GetValue<string>();
				return YAML::Load(json_str); // Parse JSON as YAML
			} catch (...) {
				return YAML::Node(value.ToString());
			}
		}
		return YAML::Node(value.ToString());
	}
	case LogicalTypeId::BOOLEAN:
		return YAML::Node(value.GetValue<bool>());
	case LogicalTypeId::TINYINT:
		return YAML::Node(static_cast<int>(value.GetValue<int8_t>()));
	case LogicalTypeId::SMALLINT:
		return YAML::Node(static_cast<int>(value.GetValue<int16_t>()));
	case LogicalTypeId::INTEGER:
		return YAML::Node(value.GetValue<int32_t>());
	case LogicalTypeId::BIGINT:
		return YAML::Node(value.GetValue<int64_t>());
	case LogicalTypeId::FLOAT:
		return YAML::Node(value.GetValue<float>());
	case LogicalTypeId::DOUBLE:
		return YAML::Node(value.GetValue<double>());
	case LogicalTypeId::LIST: {
		YAML::Node list_node(YAML::NodeType::Sequence);
		const auto &list_vals = ListValue::GetChildren(value);
		for (const auto &element : list_vals) {
			list_node.push_back(ValueToYAMLNodeImpl(element, budget)); // Recursive
		}
		return list_node;
	}
	case LogicalTypeId::STRUCT: {
		YAML::Node map_node(YAML::NodeType::Map);
		const auto &struct_vals = StructValue::GetChildren(value);
		const auto &struct_names = StructType::GetChildTypes(value.type());

		for (idx_t field_idx = 0; field_idx < struct_vals.size() && field_idx < struct_names.size(); field_idx++) {
			const string key = CompatIdentifierName(struct_names[field_idx].first);
			map_node[key] = ValueToYAMLNodeImpl(struct_vals[field_idx], budget); // Recursive
		}
		return map_node;
	}
	default:
		return YAML::Node(value.ToString());
	}
}

YAML::Node ValueToYAMLNode(const Value &value) {
	YAMLTraversalBudget budget;
	return ValueToYAMLNodeImpl(value, budget);
}

std::string ValueToYAMLString(const Value &value, YAMLFormat format, YAMLStringStyle string_style, idx_t indent) {
	try {
		// Convert to YAML::Node first (this respects emitter configuration)
		YAML::Node node = ValueToYAMLNode(value);

		// Now emit with proper format settings and string style
		YAML::Emitter out;
		ConfigureEmitter(out, format, indent);
		auto resolved = ResolveStringStyle(string_style, format);
		if (resolved == YAMLStringStyle::LITERAL) {
			EmitNodeWithStringStyle(out, node, resolved);
		} else {
			out << node;
		}

		// Check if we have a valid YAML string
		if (out.good() && out.c_str() != nullptr) {
			return out.c_str();
		} else {
			// If emitter is in error state, return null as fallback
			return "null";
		}
	} catch (const std::exception &e) {
		// Handle known exceptions
		return "null";
	} catch (...) {
		// Handle any other unexpected errors
		return "null";
	}
}

std::string FormatPerStyleAndLayout(const Value &value, YAMLFormat format, const std::string &layout) {
	// Simply return the formatted YAML string - let callers handle layout-specific formatting
	return ValueToYAMLString(value, format);
}

} // namespace yaml_utils

} // namespace duckdb
