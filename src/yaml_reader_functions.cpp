#include "json_scan.hpp"
#include "yaml_reader.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb_compat.hpp"
#include "yaml_types.hpp"
#include "yaml_utils.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include <unordered_set>

namespace duckdb {

// Helper function to parse multi_document parameter (bool or string) into MultiDocumentMode
static MultiDocumentMode ParseMultiDocumentMode(const Value &value) {
	if (value.type().id() == LogicalTypeId::BOOLEAN) {
		return value.GetValue<bool>() ? MultiDocumentMode::ROWS : MultiDocumentMode::FIRST;
	}

	if (value.type().id() == LogicalTypeId::VARCHAR) {
		string mode_str = StringUtil::Lower(value.ToString());
		if (mode_str == "rows" || mode_str == "true") {
			return MultiDocumentMode::ROWS;
		} else if (mode_str == "first" || mode_str == "false") {
			return MultiDocumentMode::FIRST;
		} else if (mode_str == "frontmatter") {
			return MultiDocumentMode::FRONTMATTER;
		} else if (mode_str == "list") {
			return MultiDocumentMode::LIST;
		} else {
			throw BinderException("Invalid multi_document mode '%s'. Valid values are: true, false, 'rows', 'first', "
			                      "'frontmatter', 'list'",
			                      mode_str);
		}
	}

	throw BinderException("multi_document parameter must be a boolean or string");
}

// Bind data structure for read_yaml
// Bind data structure for read_yaml (immutable after bind)
struct YAMLReadRowsBindData : public TableFunctionData {
	bool values = false;
	YAMLReadRowsBindData(vector<string> files, YAMLReader::YAMLReadOptions options)
	    : files(std::move(files)), options(options) {
	}

	vector<string> files;
	YAMLReader::YAMLReadOptions options;
	vector<string> names;
	vector<LogicalType> types;

	// FRONTMATTER mode: metadata column info from first document
	vector<string> frontmatter_names;
	vector<LogicalType> frontmatter_types;
};

// Bind data structure for read_yaml_objects (immutable after bind)
struct YAMLReadBindData : public TableFunctionData {
	YAMLReadBindData(vector<string> files, YAMLReader::YAMLReadOptions options)
	    : files(std::move(files)), options(options) {
	}

	vector<string> files;
	YAMLReader::YAMLReadOptions options;
	vector<string> names;
	vector<LogicalType> types;
};

unique_ptr<FunctionData> YAMLReader::YAMLReadRowsBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<CompatName> &names) {
	// Validate primary input
	if (input.inputs.empty()) {
		throw BinderException("read_yaml requires a file path parameter");
	}

	// Extract the file path parameter
	Value path_value = input.inputs[0];

	YAMLReadOptions options;

	// Check for duplicate parameters
	std::unordered_set<std::string> seen_parameters;
	for (auto &param : input.named_parameters) {
		auto param_name = CompatIdentifierName(param.first);
		if (seen_parameters.find(param_name) != seen_parameters.end()) {
			throw BinderException("Duplicate parameter name: " + param_name);
		}
		seen_parameters.insert(param_name);
	}

	// Check for columns parameter
	if (seen_parameters.find("columns") != seen_parameters.end()) {
		// Bind column types
		BindColumnTypes(context, input, options);
	}

	// Parse optional parameters
	if (seen_parameters.find("auto_detect") != seen_parameters.end()) {
		options.auto_detect_types = input.named_parameters["auto_detect"].GetValue<bool>();
	}
	if (seen_parameters.find("ignore_errors") != seen_parameters.end()) {
		options.ignore_errors = input.named_parameters["ignore_errors"].GetValue<bool>();
	}
	if (seen_parameters.find("maximum_object_size") != seen_parameters.end()) {
		options.maximum_object_size = input.named_parameters["maximum_object_size"].GetValue<int64_t>();
		if (options.maximum_object_size <= 0) {
			throw BinderException("maximum_object_size must be a positive integer");
		}
	} else if (seen_parameters.find("maximum_file_size") != seen_parameters.end()) {
		options.maximum_object_size = input.named_parameters["maximum_file_size"].GetValue<int64_t>();
		if (options.maximum_object_size <= 0) {
			throw BinderException("maximum_file_size must be a positive integer");
		}
	}
	if (seen_parameters.find("multi_document") != seen_parameters.end()) {
		options.multi_document_mode = ParseMultiDocumentMode(input.named_parameters["multi_document"]);
	}
	if (seen_parameters.find("expand_root_sequence") != seen_parameters.end()) {
		options.expand_root_sequence = input.named_parameters["expand_root_sequence"].GetValue<bool>();
	}
	if (seen_parameters.find("frontmatter_as_columns") != seen_parameters.end()) {
		options.frontmatter_as_columns = input.named_parameters["frontmatter_as_columns"].GetValue<bool>();
	}
	if (seen_parameters.find("list_column_name") != seen_parameters.end()) {
		options.list_column_name = input.named_parameters["list_column_name"].GetValue<string>();
		if (options.list_column_name.empty()) {
			throw BinderException("list_column_name cannot be empty");
		}
	}
	if (seen_parameters.find("sample_size") != seen_parameters.end()) {
		auto arg = input.named_parameters["sample_size"].GetValue<int64_t>();
		if (arg == -1) {
			options.sample_size = NumericLimits<idx_t>::Maximum();
		} else if (arg > 0) {
			options.sample_size = static_cast<idx_t>(arg);
		} else {
			throw BinderException("read_yaml \"sample_size\" parameter must be positive, or -1 to sample all input");
		}
	}
	if (seen_parameters.find("maximum_sample_files") != seen_parameters.end()) {
		auto arg = input.named_parameters["maximum_sample_files"].GetValue<int64_t>();
		if (arg == -1) {
			options.maximum_sample_files = NumericLimits<idx_t>::Maximum();
		} else if (arg > 0) {
			options.maximum_sample_files = static_cast<idx_t>(arg);
		} else {
			throw BinderException(
			    "read_yaml \"maximum_sample_files\" parameter must be positive, or -1 to remove the limit");
		}
	}
	if (seen_parameters.find("records") != seen_parameters.end()) {
		options.records_path = input.named_parameters["records"].GetValue<string>();
		if (options.records_path.empty()) {
			throw BinderException("read_yaml \"records\" parameter cannot be an empty string");
		}
		// When using records path, we don't expand root sequences (the records path points to the sequence)
		options.expand_root_sequence = false;
	}
	if (seen_parameters.find("strip_document_suffixes") != seen_parameters.end()) {
		options.strip_document_suffixes = input.named_parameters["strip_document_suffixes"].GetValue<bool>();
	}

	// Get files using value processing
	auto files = GetFiles(context, path_value, options.ignore_errors);
	if (files.empty() && !options.ignore_errors) {
		throw IOException("No YAML files found matching the input path");
	}

	// Create bind data with file list
	auto result = make_uniq<YAMLReadRowsBindData>(files, options);

	// Sample files for schema detection
	vector<YAML::Node> sample_nodes;
	vector<string> sample_sources;
	vector<YAML::Node> sample_docs; // for FRONTMATTER / LIST mode
	idx_t sampled_rows = 0;
	idx_t sampled_files = 0;

	for (const auto &current_file : files) {
		try {
			auto docs = ReadYAMLFile(context, current_file, options);

			if (options.multi_document_mode == MultiDocumentMode::LIST ||
			    options.multi_document_mode == MultiDocumentMode::FRONTMATTER) {
				sample_docs.insert(sample_docs.end(), docs.begin(), docs.end());
			}

			vector<YAML::Node> file_nodes;
			if (!options.records_path.empty()) {
				for (const auto &doc : docs) {
					YAML::Node records_node = NavigateToPath(doc, options.records_path);
					if (records_node.Type() == YAML::NodeType::Undefined ||
					    records_node.Type() == YAML::NodeType::Null || !records_node.IsDefined()) {
						if (!options.ignore_errors) {
							throw BinderException("Records path '" + options.records_path +
							                      "' not found in YAML document");
						}
						continue;
					}
					if (!records_node.IsSequence()) {
						if (!options.ignore_errors) {
							throw BinderException("Records path '" + options.records_path +
							                      "' does not point to a sequence/array");
						}
						continue;
					}
					for (size_t idx = 0; idx < records_node.size(); idx++) {
						if (records_node[idx].IsMap()) {
							file_nodes.push_back(records_node[idx]);
						}
					}
				}
			} else if (options.multi_document_mode == MultiDocumentMode::ROWS ||
			           options.multi_document_mode == MultiDocumentMode::FIRST) {
				file_nodes = ExtractRowNodes(docs, options.expand_root_sequence);
			}

			if (options.multi_document_mode == MultiDocumentMode::ROWS ||
			    options.multi_document_mode == MultiDocumentMode::FIRST) {
				for (const auto &node : file_nodes) {
					if (sampled_rows >= options.sample_size) {
						break;
					}
					sample_nodes.push_back(node);
					sample_sources.push_back(current_file);
					sampled_rows++;
				}
				sampled_files++;
				if (sampled_files >= options.maximum_sample_files || sampled_rows >= options.sample_size) {
					break;
				}
			}
		} catch (const std::exception &e) {
			if (!options.ignore_errors) {
				throw;
			}
		}
	}

	// Mode-specific schema handling
	if (options.multi_document_mode == MultiDocumentMode::FRONTMATTER) {
		if (sample_docs.size() < 2) {
			if (!options.ignore_errors) {
				throw BinderException("FRONTMATTER mode requires at least 2 documents (frontmatter + data)");
			}
		} else {
			YAML::Node frontmatter = sample_docs[0];
			if (options.frontmatter_as_columns) {
				if (frontmatter.IsMap()) {
					for (auto it = frontmatter.begin(); it != frontmatter.end(); ++it) {
						string key = "meta_" + it->first.Scalar();
						LogicalType type;
						if (options.auto_detect_types) {
							type = DetectYAMLType(context, it->second);
						} else {
							type = LogicalType::VARCHAR;
						}
						result->frontmatter_names.push_back(key);
						result->frontmatter_types.push_back(type);
						names.push_back(CompatMakeName(key));
						return_types.push_back(type);
					}
				}
			} else {
				names.push_back("frontmatter");
				return_types.push_back(YAMLTypes::YAMLType());
				result->frontmatter_names.push_back("frontmatter");
				result->frontmatter_types.push_back(YAMLTypes::YAMLType());
			}

			vector<YAML::Node> data_docs(sample_docs.begin() + 1, sample_docs.end());
			auto row_nodes = ExtractRowNodes(data_docs, options.expand_root_sequence);
			for (const auto &node : row_nodes) {
				if (sampled_rows >= options.sample_size) {
					break;
				}
				sample_nodes.push_back(node);
				sampled_rows++;
			}
		}
	} else if (options.multi_document_mode == MultiDocumentMode::LIST) {
		for (const auto &doc : sample_docs) {
			if (sampled_rows >= options.sample_size) {
				break;
			}
			sample_nodes.push_back(doc);
			sampled_rows++;
		}
		LogicalType list_element_type = DetectJaggedYAMLType(context, sample_nodes);
		names.push_back(CompatMakeName(options.list_column_name));
		return_types.push_back(LogicalType::LIST(list_element_type));
		result->names = CompatNameStrings(names);
		result->types = return_types;
		return std::move(result);
	}

	// Handle empty sample result set early
	if (sample_nodes.empty()) {
		if (options.ignore_errors) {
			if (names.empty()) {
				names.emplace_back("yaml");
				return_types.emplace_back(LogicalType::VARCHAR);
			}
		} else {
			throw IOException("No valid YAML documents found");
		}
		result->names = CompatNameStrings(names);
		result->types = return_types;
		return std::move(result);
	}

	vector<Identifier> inferred_names;
	vector<LogicalType> inferred_types;
	result->values = DetectYAMLColumns(context, sample_nodes, inferred_types, inferred_names, sample_sources);
	for (idx_t col = 0; col < inferred_names.size(); col++) {
		auto column_type = options.auto_detect_types ? inferred_types[col] : LogicalType::VARCHAR;
		for (idx_t i = 0; i < options.column_names.size(); i++) {
			if (options.column_names[i] == inferred_names[col].GetIdentifierName()) {
				column_type = options.column_types[i];
				break;
			}
		}
		names.push_back(inferred_names[col]);
		return_types.push_back(column_type);
	}

	// Save schema
	result->names = CompatNameStrings(names);
	JSONScan::DeduplicateColumnNames(names);
	result->types = return_types;

	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> YAMLReader::YAMLReadRowsInit(ClientContext &context,
                                                                  TableFunctionInitInput &input) {
	auto result = make_uniq<YAMLReadGlobalState>();
	auto &bind_data = input.bind_data->Cast<YAMLReadRowsBindData>();
	result->files = bind_data.files;
	return std::move(result);
}

unique_ptr<LocalTableFunctionState> YAMLReader::YAMLReadRowsInitLocal(ExecutionContext &context,
                                                                      TableFunctionInitInput &input,
                                                                      GlobalTableFunctionState *global_state) {
	return make_uniq<YAMLReadLocalState>();
}

unique_ptr<GlobalTableFunctionState> YAMLReader::YAMLReadObjectsInit(ClientContext &context,
                                                                     TableFunctionInitInput &input) {
	auto result = make_uniq<YAMLReadGlobalState>();
	auto &bind_data = input.bind_data->Cast<YAMLReadBindData>();
	result->files = bind_data.files;
	return std::move(result);
}

unique_ptr<LocalTableFunctionState> YAMLReader::YAMLReadObjectsInitLocal(ExecutionContext &context,
                                                                         TableFunctionInitInput &input,
                                                                         GlobalTableFunctionState *global_state) {
	return make_uniq<YAMLReadLocalState>();
}

OperatorPartitionData YAMLReader::YAMLReadGetPartitionData(ClientContext &context,
                                                           TableFunctionGetPartitionInput &input) {
	auto &lstate = input.local_state->Cast<YAMLReadLocalState>();
	return OperatorPartitionData(lstate.last_batch_index);
}

unique_ptr<FunctionData> YAMLReader::YAMLReadObjectsBind(ClientContext &context, TableFunctionBindInput &input,
                                                         vector<LogicalType> &return_types, vector<CompatName> &names) {
	// Validate primary input
	if (input.inputs.empty()) {
		throw BinderException("read_yaml_objects requires a file path parameter");
	}

	Value path_value = input.inputs[0];
	YAMLReadOptions options;

	// Check for duplicate parameters
	std::unordered_set<std::string> seen_parameters;
	for (auto &param : input.named_parameters) {
		auto param_name = CompatIdentifierName(param.first);
		if (seen_parameters.find(param_name) != seen_parameters.end()) {
			throw BinderException("Duplicate parameter name: " + param_name);
		}
		seen_parameters.insert(param_name);
	}

	if (seen_parameters.find("columns") != seen_parameters.end()) {
		BindColumnTypes(context, input, options);
	}

	if (seen_parameters.find("auto_detect") != seen_parameters.end()) {
		options.auto_detect_types = input.named_parameters["auto_detect"].GetValue<bool>();
	}
	if (seen_parameters.find("ignore_errors") != seen_parameters.end()) {
		options.ignore_errors = input.named_parameters["ignore_errors"].GetValue<bool>();
	}
	if (seen_parameters.find("maximum_object_size") != seen_parameters.end()) {
		options.maximum_object_size = input.named_parameters["maximum_object_size"].GetValue<int64_t>();
		if (options.maximum_object_size <= 0) {
			throw BinderException("maximum_object_size must be a positive integer");
		}
	} else if (seen_parameters.find("maximum_file_size") != seen_parameters.end()) {
		options.maximum_object_size = input.named_parameters["maximum_file_size"].GetValue<int64_t>();
		if (options.maximum_object_size <= 0) {
			throw BinderException("maximum_file_size must be a positive integer");
		}
	}
	if (seen_parameters.find("multi_document") != seen_parameters.end()) {
		options.multi_document_mode = ParseMultiDocumentMode(input.named_parameters["multi_document"]);
	}
	if (seen_parameters.find("expand_root_sequence") != seen_parameters.end()) {
		options.expand_root_sequence = input.named_parameters["expand_root_sequence"].GetValue<bool>();
	}
	if (seen_parameters.find("sample_size") != seen_parameters.end()) {
		auto arg = input.named_parameters["sample_size"].GetValue<int64_t>();
		if (arg == -1) {
			options.sample_size = NumericLimits<idx_t>::Maximum();
		} else if (arg > 0) {
			options.sample_size = static_cast<idx_t>(arg);
		} else {
			throw BinderException(
			    "read_yaml_objects \"sample_size\" parameter must be positive, or -1 to sample all input");
		}
	}
	if (seen_parameters.find("maximum_sample_files") != seen_parameters.end()) {
		auto arg = input.named_parameters["maximum_sample_files"].GetValue<int64_t>();
		if (arg == -1) {
			options.maximum_sample_files = NumericLimits<idx_t>::Maximum();
		} else if (arg > 0) {
			options.maximum_sample_files = static_cast<idx_t>(arg);
		} else {
			throw BinderException(
			    "read_yaml_objects \"maximum_sample_files\" parameter must be positive, or -1 to remove the limit");
		}
	}
	if (seen_parameters.find("strip_document_suffixes") != seen_parameters.end()) {
		options.strip_document_suffixes = input.named_parameters["strip_document_suffixes"].GetValue<bool>();
	}

	auto files = GetFiles(context, path_value, options.ignore_errors);
	if (files.empty() && !options.ignore_errors) {
		throw IOException("No YAML files found matching the input path");
	}

	auto result = make_uniq<YAMLReadBindData>(files, options);

	vector<YAML::Node> sample_docs;
	vector<string> sample_sources;
	idx_t sampled_rows = 0;
	idx_t sampled_files = 0;

	for (const auto &file_path : files) {
		try {
			auto docs = ReadYAMLFile(context, file_path, options);
			if (sampled_files < options.maximum_sample_files && sampled_rows < options.sample_size) {
				for (const auto &doc : docs) {
					if (sampled_rows >= options.sample_size) {
						break;
					}
					sample_docs.push_back(doc);
					sample_sources.push_back(file_path);
					sampled_rows++;
				}
				sampled_files++;
			}
			if (sampled_files >= options.maximum_sample_files || sampled_rows >= options.sample_size) {
				break;
			}
		} catch (const std::exception &e) {
			if (!options.ignore_errors) {
				throw;
			}
		}
	}

	if (sample_docs.empty()) {
		if (!options.column_names.empty()) {
			names = CompatMakeNames(options.column_names);
			return_types = options.column_types;
		} else {
			names.emplace_back("yaml");
			return_types.emplace_back(LogicalType::VARCHAR);
		}
		result->names = CompatNameStrings(names);
		result->types = return_types;
		return std::move(result);
	}

	if (!options.column_names.empty()) {
		names = CompatMakeNames(options.column_names);
		return_types = options.column_types;
	} else {
		if (options.auto_detect_types) {
			names.emplace_back("yaml");
			auto doc_type = DetectJaggedYAMLType(context, sample_docs, sample_sources);
			return_types.emplace_back(doc_type);
		} else {
			names.emplace_back("yaml");
			return_types.emplace_back(LogicalType::VARCHAR);
		}
	}

	result->names = CompatNameStrings(names);
	result->types = return_types;
	return std::move(result);
}

static idx_t DocumentIndex(const YAMLReadLocalState &state, const YAML::Node &node) {
	idx_t index = 1;
	for (idx_t i = 0; i < state.document_marks.size(); i++) {
		const auto &mark = state.document_marks[i];
		if (mark.pos >= 0 && mark.pos <= node.Mark().pos) {
			index = i + 1;
		}
	}
	return index;
}

void YAMLReader::YAMLReadRowsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	const auto &bind_data = data_p.bind_data->Cast<YAMLReadRowsBindData>();
	auto &gstate = data_p.global_state->Cast<YAMLReadGlobalState>();
	auto &lstate = data_p.local_state->Cast<YAMLReadLocalState>();

	idx_t output_idx = 0;
	output.Reset();

	while (output_idx < STANDARD_VECTOR_SIZE) {
		// Ensure this worker owns a file; claim the next one otherwise.
		if (!lstate.have_file) {
			idx_t claimed = gstate.ClaimNextFile();
			if (claimed == DConstants::INVALID_INDEX) {
				break; // No more files
			}
			lstate.file_index = claimed;
			lstate.current_filename = gstate.files[claimed];
			lstate.chunk_counter = 0;
			lstate.have_file = true;
			lstate.file_loaded = false;
		}

		const auto &filename = lstate.current_filename;
		bool file_done = false;

		try {
			if (!lstate.file_loaded) {
				auto docs = ReadYAMLFile(context, filename, bind_data.options, &lstate.document_marks);

				if (bind_data.options.multi_document_mode == MultiDocumentMode::FRONTMATTER) {
					if (docs.size() < 2) {
						if (!bind_data.options.ignore_errors) {
							throw BinderException(
							    "FRONTMATTER mode requires at least 2 documents (frontmatter + data)");
						}
						lstate.file_nodes.clear();
					} else {
						YAML::Node fm = docs[0];
						lstate.frontmatter_values.clear();
						if (bind_data.options.frontmatter_as_columns) {
							if (fm.IsMap()) {
								for (idx_t i = 0; i < bind_data.frontmatter_names.size(); i++) {
									string raw_key = bind_data.frontmatter_names[i].substr(5); // strip "meta_"
									YAML::Node val = fm[raw_key];
									if (val) {
										lstate.frontmatter_values.push_back(
										    YAMLNodeToValue(val, bind_data.frontmatter_types[i]));
									} else {
										lstate.frontmatter_values.push_back(Value(bind_data.frontmatter_types[i]));
									}
								}
							} else {
								for (idx_t i = 0; i < bind_data.frontmatter_names.size(); i++) {
									lstate.frontmatter_values.push_back(Value(bind_data.frontmatter_types[i]));
								}
							}
						} else {
							lstate.frontmatter_values.push_back(YAMLNodeToValue(fm, YAMLTypes::YAMLType()));
						}

						vector<YAML::Node> data_docs(docs.begin() + 1, docs.end());
						lstate.file_nodes = ExtractRowNodes(data_docs, bind_data.options.expand_root_sequence);
					}
				} else if (bind_data.options.multi_document_mode == MultiDocumentMode::LIST) {
					lstate.file_nodes = std::move(docs);
					lstate.list_mode_done = false;
				} else if (!bind_data.options.records_path.empty()) {
					vector<YAML::Node> file_nodes;
					for (const auto &doc : docs) {
						YAML::Node records_node = NavigateToPath(doc, bind_data.options.records_path);
						if (records_node.Type() == YAML::NodeType::Undefined ||
						    records_node.Type() == YAML::NodeType::Null || !records_node.IsDefined()) {
							if (!bind_data.options.ignore_errors) {
								throw BinderException("Records path '" + bind_data.options.records_path +
								                      "' not found in YAML document");
							}
							continue;
						}
						if (!records_node.IsSequence()) {
							if (!bind_data.options.ignore_errors) {
								throw BinderException("Records path '" + bind_data.options.records_path +
								                      "' does not point to a sequence/array");
							}
							continue;
						}
						for (size_t idx = 0; idx < records_node.size(); idx++) {
							if (records_node[idx].IsMap()) {
								file_nodes.push_back(records_node[idx]);
							}
						}
					}
					lstate.file_nodes = std::move(file_nodes);
				} else {
					lstate.file_nodes = ExtractRowNodes(docs, bind_data.options.expand_root_sequence);
				}

				lstate.current_row_index = 0;
				lstate.file_loaded = true;
			}

			if (bind_data.options.multi_document_mode == MultiDocumentMode::LIST) {
				if (!lstate.list_mode_done) {
					vector<Value> doc_values;
					LogicalType element_type;
					if (bind_data.types[0].id() == LogicalTypeId::LIST) {
						element_type = ListType::GetChildType(bind_data.types[0]);
					} else {
						element_type = bind_data.types[0];
					}
					for (const auto &doc : lstate.file_nodes) {
						try {
							doc_values.push_back(YAMLNodeToValue(doc, element_type, bind_data.options.ignore_errors));
						} catch (const OutOfMemoryException &) {
							throw;
						} catch (const std::exception &e) {
							if (!bind_data.options.ignore_errors) {
								throw IOException("YAML file '%s', document %llu: %s", filename,
								                  (unsigned long long)DocumentIndex(lstate, doc),
								                  ErrorData(e).RawMessage());
							}
						}
					}
					Value list_value = Value::LIST(element_type, doc_values);
					output.SetValue(0, output_idx, list_value);
					output_idx++;
					lstate.list_mode_done = true;
				}
				file_done = true;
			} else {
				idx_t fm_col_count = lstate.frontmatter_values.size();

				while (lstate.current_row_index < lstate.file_nodes.size() && output_idx < STANDARD_VECTOR_SIZE) {
					const auto &node = lstate.file_nodes[lstate.current_row_index];
					try {
						if (bind_data.values) {
							Value val = YAMLNodeToValue(node, bind_data.types[0], bind_data.options.ignore_errors);
							output.SetValue(0, output_idx, val);
						} else {
							idx_t col_idx = 0;
							if (bind_data.options.multi_document_mode == MultiDocumentMode::FRONTMATTER) {
								for (idx_t fm_idx = 0; fm_idx < fm_col_count; fm_idx++) {
									output.SetValue(col_idx, output_idx, lstate.frontmatter_values[fm_idx]);
									col_idx++;
								}
							}
							for (; col_idx < bind_data.names.size(); col_idx++) {
								const string &col_name = bind_data.names[col_idx];
								const LogicalType &type = bind_data.types[col_idx];
								YAML::Node value = node[col_name];
								Value duckdb_value;
								if (value) {
									duckdb_value = YAMLNodeToValue(value, type, bind_data.options.ignore_errors);
								} else {
									duckdb_value = Value(type);
								}
								output.SetValue(col_idx, output_idx, duckdb_value);
							}
						}

					} catch (const OutOfMemoryException &) {
						throw;
					} catch (const std::exception &e) {
						if (!bind_data.options.ignore_errors) {
							throw IOException("YAML file '%s', document %llu: %s", filename,
							                  (unsigned long long)DocumentIndex(lstate, node),
							                  ErrorData(e).RawMessage());
						}
						lstate.current_row_index++;
						continue;
					}

					output_idx++;
					lstate.current_row_index++;
				}

				if (lstate.current_row_index >= lstate.file_nodes.size()) {
					file_done = true;
				}
			}
		} catch (const OutOfMemoryException &) {
			throw;
		} catch (const Exception &e) {
			if (!bind_data.options.ignore_errors) {
				throw;
			}
			lstate.ResetFileResources();
			lstate.have_file = false;
			continue;
		} catch (const std::exception &e) {
			if (!bind_data.options.ignore_errors) {
				throw IOException("Error processing YAML file '" + filename + "': " + ErrorData(e).RawMessage());
			}
			lstate.ResetFileResources();
			lstate.have_file = false;
			continue;
		}

		if (file_done) {
			lstate.ResetFileResources();
			lstate.have_file = false;
			if (output_idx > 0) {
				lstate.last_batch_index = (lstate.file_index << YAMLReadLocalState::FILE_SHIFT) | lstate.chunk_counter;
				lstate.chunk_counter++;
				break;
			}
			continue;
		}

		if (output_idx >= STANDARD_VECTOR_SIZE) {
			lstate.last_batch_index = (lstate.file_index << YAMLReadLocalState::FILE_SHIFT) | lstate.chunk_counter;
			lstate.chunk_counter++;
			break;
		}
	}

	CompatSetOutputCardinality(output, output_idx);
}

void YAMLReader::YAMLReadObjectsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	const auto &bind_data = data_p.bind_data->Cast<YAMLReadBindData>();
	auto &gstate = data_p.global_state->Cast<YAMLReadGlobalState>();
	auto &lstate = data_p.local_state->Cast<YAMLReadLocalState>();

	idx_t output_idx = 0;
	output.Reset();

	while (output_idx < STANDARD_VECTOR_SIZE) {
		if (!lstate.have_file) {
			idx_t claimed = gstate.ClaimNextFile();
			if (claimed == DConstants::INVALID_INDEX) {
				break;
			}
			lstate.file_index = claimed;
			lstate.current_filename = gstate.files[claimed];
			lstate.chunk_counter = 0;
			lstate.have_file = true;
			lstate.file_loaded = false;
		}

		const auto &filename = lstate.current_filename;
		bool file_done = false;

		try {
			if (!lstate.file_loaded) {
				auto docs = ReadYAMLFile(context, filename, bind_data.options, &lstate.document_marks);

				lstate.file_nodes = std::move(docs);
				lstate.current_row_index = 0;
				lstate.file_loaded = true;
			}

			while (lstate.current_row_index < lstate.file_nodes.size() && output_idx < STANDARD_VECTOR_SIZE) {
				const auto &node = lstate.file_nodes[lstate.current_row_index];
				try {
					if (bind_data.names.size() == 1 && bind_data.names[0] == "yaml") {
						Value val = YAMLNodeToValue(node, bind_data.types[0], bind_data.options.ignore_errors);
						output.SetValue(0, output_idx, val);
					} else {
						if (node.IsMap()) {
							for (idx_t col_idx = 0; col_idx < bind_data.names.size(); col_idx++) {
								const string &col_name = bind_data.names[col_idx];
								const LogicalType &col_type = bind_data.types[col_idx];
								YAML::Node value = node[col_name];
								Value duckdb_value;
								if (value) {
									duckdb_value = YAMLNodeToValue(value, col_type, bind_data.options.ignore_errors);
								} else {
									duckdb_value = Value(col_type);
								}
								output.SetValue(col_idx, output_idx, duckdb_value);
							}
						} else if (bind_data.types.size() == 1) {
							Value val = YAMLNodeToValue(node, bind_data.types[0], bind_data.options.ignore_errors);
							output.SetValue(0, output_idx, val);
						}
					}

				} catch (const OutOfMemoryException &) {
					throw;
				} catch (const std::exception &e) {
					if (!bind_data.options.ignore_errors) {
						throw IOException("YAML file '%s', document %llu: %s", filename,
						                  (unsigned long long)DocumentIndex(lstate, node), ErrorData(e).RawMessage());
					}
					lstate.current_row_index++;
					continue;
				}

				output_idx++;
				lstate.current_row_index++;
			}

			if (lstate.current_row_index >= lstate.file_nodes.size()) {
				file_done = true;
			}

		} catch (const OutOfMemoryException &) {
			throw;
		} catch (const Exception &e) {
			if (!bind_data.options.ignore_errors) {
				throw;
			}
			lstate.ResetFileResources();
			lstate.have_file = false;
			continue;
		} catch (const std::exception &e) {
			if (!bind_data.options.ignore_errors) {
				throw IOException("Error processing YAML file '" + filename + "': " + ErrorData(e).RawMessage());
			}
			lstate.ResetFileResources();
			lstate.have_file = false;
			continue;
		}

		if (file_done) {
			lstate.ResetFileResources();
			lstate.have_file = false;
			if (output_idx > 0) {
				lstate.last_batch_index = (lstate.file_index << YAMLReadLocalState::FILE_SHIFT) | lstate.chunk_counter;
				lstate.chunk_counter++;
				break;
			}
			continue;
		}

		if (output_idx >= STANDARD_VECTOR_SIZE) {
			lstate.last_batch_index = (lstate.file_index << YAMLReadLocalState::FILE_SHIFT) | lstate.chunk_counter;
			lstate.chunk_counter++;
			break;
		}
	}

	CompatSetOutputCardinality(output, output_idx);
}

//===--------------------------------------------------------------------===//
// parse_yaml Table Function - Parse YAML strings into rows
//===--------------------------------------------------------------------===//

// Bind data structure for parse_yaml (immutable after bind)
struct ParseYAMLBindData : public TableFunctionData {
	bool values = false;
	ParseYAMLBindData() = default;

	vector<YAML::Node> yaml_docs;                                    // Parsed YAML documents
	vector<string> names;                                            // Column names
	vector<LogicalType> types;                                       // Column types
	MultiDocumentMode multi_document_mode = MultiDocumentMode::ROWS; // How to handle multi-document YAML
	bool expand_root_sequence = true;                                // Whether to expand top-level sequences
	bool frontmatter_as_columns = true;                              // For FRONTMATTER mode
	string list_column_name = "documents";                           // For LIST mode
};

// Local state for parse_yaml (mutable execution state)
struct ParseYAMLLocalState : public LocalTableFunctionState {
	idx_t current_row = 0;
};

unique_ptr<FunctionData> YAMLReader::ParseYAMLBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<CompatName> &names) {
	if (input.inputs.empty()) {
		throw BinderException("parse_yaml requires a YAML string parameter");
	}

	// Get the YAML string
	Value yaml_value = input.inputs[0];
	if (yaml_value.IsNull()) {
		throw BinderException("parse_yaml input cannot be NULL");
	}

	string yaml_str = yaml_value.ToString();

	auto result = make_uniq<ParseYAMLBindData>();

	// Parse optional parameters
	for (auto &param : input.named_parameters) {
		if (param.first == "multi_document") {
			result->multi_document_mode = ParseMultiDocumentMode(param.second);
		} else if (param.first == "expand_root_sequence") {
			result->expand_root_sequence = param.second.GetValue<bool>();
		} else if (param.first == "frontmatter_as_columns") {
			result->frontmatter_as_columns = param.second.GetValue<bool>();
		} else if (param.first == "list_column_name") {
			result->list_column_name = param.second.GetValue<string>();
			if (result->list_column_name.empty()) {
				throw BinderException("list_column_name cannot be empty");
			}
		}
	}

	// Parse the YAML string
	try {
		vector<YAML::Node> docs;
		if (result->multi_document_mode != MultiDocumentMode::FIRST) {
			std::stringstream ss(yaml_str);
			docs = YAML::LoadAll(ss);
		} else {
			docs.push_back(YAML::Load(yaml_str));
		}

		for (auto &doc : docs) {
			doc.reset(yaml_utils::ExpandMerges(doc));
		}

		// Extract row nodes (expand sequences if needed)
		result->yaml_docs = ExtractRowNodes(docs, result->expand_root_sequence);
	} catch (const YAML::Exception &e) {
		throw InvalidInputException("Failed to parse YAML: %s", ErrorData(e).RawMessage());
	}

	if (result->yaml_docs.empty()) {
		// Empty result - return a single yaml column
		names.emplace_back("yaml");
		return_types.emplace_back(LogicalType::VARCHAR);
		result->names = CompatNameStrings(names);
		result->types = return_types;
		return std::move(result);
	}

	// Detect schema from all documents using jagged schema detection
	LogicalType merged_type = DetectJaggedYAMLType(context, result->yaml_docs);

	if (merged_type.id() == LogicalTypeId::STRUCT) {
		// Struct type - use struct fields as columns
		auto &children = StructType::GetChildTypes(merged_type);
		for (auto &child : children) {
			names.push_back(CompatMakeName(CompatIdentifierName(child.first)));
			return_types.push_back(child.second);
		}
	} else {
		// Non-struct type - return single yaml column
		result->values = true;
		names.emplace_back("yaml");
		return_types.emplace_back(merged_type);
	}

	result->names = CompatNameStrings(names);
	result->types = return_types;
	return std::move(result);
}

unique_ptr<LocalTableFunctionState> YAMLReader::ParseYAMLInit(ExecutionContext &context, TableFunctionInitInput &input,
                                                              GlobalTableFunctionState *global_state) {
	return make_uniq<ParseYAMLLocalState>();
}

void YAMLReader::ParseYAMLFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<ParseYAMLBindData>();
	auto &local_state = data_p.local_state->Cast<ParseYAMLLocalState>();

	// If we've processed all rows, we're done
	if (local_state.current_row >= bind_data.yaml_docs.size()) {
		CompatSetOutputCardinality(output, 0);
		return;
	}

	// Process up to STANDARD_VECTOR_SIZE rows at a time
	idx_t count = 0;
	idx_t max_count = std::min((idx_t)STANDARD_VECTOR_SIZE, bind_data.yaml_docs.size() - local_state.current_row);

	output.Reset();

	for (idx_t doc_idx = 0; doc_idx < max_count; doc_idx++) {
		YAML::Node node = bind_data.yaml_docs[local_state.current_row + doc_idx];

		if (!bind_data.values) {
			// Map node - process each field as a column
			for (idx_t col_idx = 0; col_idx < bind_data.names.size(); col_idx++) {
				const string &col_name = bind_data.names[col_idx];
				const LogicalType &col_type = bind_data.types[col_idx];

				if (node[col_name]) {
					Value val = YAMLNodeToValue(node[col_name], col_type);
					output.SetValue(col_idx, count, val);
				} else {
					output.SetValue(col_idx, count, Value(col_type));
				}
			}
		} else if (bind_data.types.size() == 1) {
			// Non-map node with single column output
			Value val = YAMLNodeToValue(node, bind_data.types[0]);
			output.SetValue(0, count, val);
		}
		count++;
	}

	local_state.current_row += count;
	CompatSetOutputCardinality(output, count);
}

} // namespace duckdb
