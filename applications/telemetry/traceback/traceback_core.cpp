/*
 * Copyright (c) 2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright notice, this list of
 *       conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright notice, this list of
 *       conditions and the following disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 *     * Neither the name of the NVIDIA CORPORATION nor the names of its contributors may be used
 *       to endorse or promote products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL NVIDIA CORPORATION BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TOR (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <traceback_core.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <string>
#include <vector>

#include <json-c/json.h>

#include <doca_argp.h>
#include <doca_log.h>

DOCA_LOG_REGISTER(TRACEBACK_CORE);

namespace {

doca_error_t register_argp_param(doca_argp_type type,
				 char const *short_name,
				 char const *long_name,
				 char const *description,
				 bool is_required,
				 doca_argp_param_cb_t callback) noexcept
{
	if (!short_name && !long_name) {
		DOCA_LOG_ERR("Cannot register argp parameter with no name");
		return DOCA_ERROR_INVALID_VALUE;
	}

	doca_error_t status;
	doca_argp_param *param = nullptr;

	status = doca_argp_param_create(&param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create argp parameter: %s", doca_error_get_name(status));
		return status;
	}

	if (short_name != nullptr) {
		doca_argp_param_set_short_name(param, short_name);
	}
	if (long_name != nullptr) {
		doca_argp_param_set_long_name(param, long_name);
	}
	doca_argp_param_set_description(param, description);
	doca_argp_param_set_callback(param, callback);
	doca_argp_param_set_type(param, type);
	if (is_required) {
		doca_argp_param_set_mandatory(param);
	}

	status = doca_argp_register_param(param);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to register arg parser parameter: %s. Error: %s",
			     long_name == nullptr ? short_name : long_name,
			     doca_error_get_descr(status));
		return status;
	}

	return DOCA_SUCCESS;
}

doca_error_t parse_json_data_id(json_object *data_id_json, std::vector<data_id_definition> &data_id_definitions)
{
	json_object *json_node;
	data_id_definition data_id{};
	size_t const rule_idx = data_id_definitions.size();

	if (!json_object_object_get_ex(data_id_json, "name", &json_node)) {
		DOCA_LOG_ERR("Data ID[%zu] Missing \"name\" node in the JSON data", rule_idx);
		return DOCA_ERROR_INVALID_VALUE;
	}

	data_id.name = json_object_get_string(json_node);
	DOCA_LOG_DBG("Data ID[%zu] name: %s", rule_idx, data_id.name.c_str());

	if (!json_object_object_get_ex(data_id_json, "data_id", &json_node)) {
		DOCA_LOG_ERR("Data ID[%zu] Missing \"data_id\" node in the JSON data", rule_idx);
		return DOCA_ERROR_INVALID_VALUE;
	}

	data_id.data_id = std::strtoull(json_object_get_string(json_node), nullptr, 16);
	DOCA_LOG_DBG("Data ID[%zu] data_id: 0x%0llx", rule_idx, static_cast<unsigned long long>(data_id.data_id));

	data_id_definitions.push_back(std::move(data_id));

	return DOCA_SUCCESS;
}

doca_error_t parse_json_rule_data(json_object *rule_json, std::vector<rule_definition> &rule_definitions)
{
	json_object *json_node;
	rule_definition rule{};
	size_t const rule_idx = rule_definitions.size();

	if (!json_object_object_get_ex(rule_json, "name", &json_node)) {
		DOCA_LOG_ERR("Rule[%zu] Missing \"name\" node in the JSON data", rule_idx);
		return DOCA_ERROR_INVALID_VALUE;
	}

	rule.name = json_object_get_string(json_node);
	DOCA_LOG_DBG("Rule[%zu] name: %s", rule_idx, rule.name.c_str());

	if (!json_object_object_get_ex(rule_json, "data_id", &json_node)) {
		DOCA_LOG_ERR("Rule[%zu] Missing \"data_id\" node in the JSON data", rule_idx);
		return DOCA_ERROR_INVALID_VALUE;
	}

	rule.data_id = std::strtoull(json_object_get_string(json_node), nullptr, 16);
	DOCA_LOG_DBG("Rule[%zu] data_id: 0x%0llx", rule_idx, static_cast<unsigned long long>(rule.data_id));

	if (!json_object_object_get_ex(rule_json, "comparison_type", &json_node)) {
		DOCA_LOG_ERR("rule[%zu] Missing \"comparison_type\" node in the JSON data", rule_idx);
		return DOCA_ERROR_INVALID_VALUE;
	}

	std::string const comparison_type = json_object_get_string(json_node);
	if (comparison_type == "inc_gt" || comparison_type == "incremental_greater_than") {
		rule.comparison_type = rule_comparison_type::incremental;
	} else if (comparison_type == "dt_gt" || comparison_type == "differential_greater_than") {
		rule.comparison_type = rule_comparison_type::differential_greater_than;
	} else if (comparison_type == "dt_lt" || comparison_type == "differential_less_than") {
		rule.comparison_type = rule_comparison_type::differential_less_than;
	} else if (comparison_type == "gt" || comparison_type == "greater_than") {
		rule.comparison_type = rule_comparison_type::greater_than;
	} else if (comparison_type == "lt" || comparison_type == "less_than") {
		rule.comparison_type = rule_comparison_type::less_than;
	} else {
		DOCA_LOG_ERR("Rule[%zu] \"comparison_type\" value: \"%s\" is not valid",
			     rule_idx,
			     comparison_type.c_str());
		return DOCA_ERROR_INVALID_VALUE;
	}
	DOCA_LOG_DBG("Rule[%zu] comparison_type: %s", rule_idx, comparison_type.c_str());

	if (!json_object_object_get_ex(rule_json, "comparison_value", &json_node)) {
		DOCA_LOG_ERR("Rule[%zu] Missing \"comparison_value\" node in the JSON data", rule_idx);
		return DOCA_ERROR_INVALID_VALUE;
	}

	rule.comparison_value = std::strtoul(json_object_get_string(json_node), nullptr, 10);
	DOCA_LOG_DBG("Rule[%zu] comparison_value: %llu",
		     rule_idx,
		     static_cast<unsigned long long>(rule.comparison_value));

	rule_definitions.push_back(std::move(rule));

	return DOCA_SUCCESS;
}

std::string date_string(std::chrono::system_clock::time_point time)
{
	std::string str;
	str.resize(32);
	std::time_t const c_time = std::chrono::system_clock::to_time_t(time);

	auto const final_len = std::strftime(std::addressof(str[0]), str.size() - 1, "%FT%TZ", std::gmtime(&c_time));

	str.resize(final_len);
	return str;
}

uint8_t constexpr uint_to_log_2(uint64_t value)
{
	if (value == 0)
		return -1; /* obviously invalid value, we don't have 256 bit data types */

	uint8_t ret = 0;
	value >>= 1;
	while (value) {
		value >>= 1;
		++ret;
	}

	return ret;
}

} /* namespace */

std::ostream &operator<<(std::ostream &os, rule_comparison_type comparison_type)
{
	switch (comparison_type) {
	case rule_comparison_type::incremental:
		os << "incremental";
		break;
	case rule_comparison_type::differential_greater_than:
		os << "differential_greater_than";
		break;
	case rule_comparison_type::differential_less_than:
		os << "differential_less_than";
		break;
	case rule_comparison_type::greater_than:
		os << "greater_than";
		break;
	case rule_comparison_type::less_than:
		os << "less_than";
		break;
	}

	return os;
}

doca_error_t parse_configuration(int argc, char **argv, configuration &config) noexcept
{
	/* Apply defaults for optional params */
	config.data_ids_file_path = "data-ids.json";
	config.rules_file_path = "rules.json";
	config.event_log_file_path = "events.log";
	config.force_diag_ownership = false;

	doca_error_t status;
	status = doca_argp_init(argv[0], &config);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init argp: %s", doca_error_get_name(status));
		return status;
	}

	status = register_argp_param(DOCA_ARGP_TYPE_STRING,
				     "d",
				     "device",
				     "Device identifier",
				     true,
				     [](void *value, void *cfg) noexcept {
					     static_cast<configuration *>(cfg)->device_id =
						     static_cast<char const *>(value);
					     return DOCA_SUCCESS;
				     });
	if (status != DOCA_SUCCESS) {
		static_cast<void>(doca_argp_destroy());
		DOCA_LOG_ERR("Failed to register device argp param: %s", doca_error_get_name(status));
		return status;
	}

	status = register_argp_param(DOCA_ARGP_TYPE_STRING,
				     nullptr,
				     "data-ids",
				     "Path to JSON file containing data ID definitions",
				     false,
				     [](void *value, void *cfg) noexcept {
					     static_cast<configuration *>(cfg)->data_ids_file_path =
						     static_cast<char const *>(value);
					     return DOCA_SUCCESS;
				     });
	if (status != DOCA_SUCCESS) {
		static_cast<void>(doca_argp_destroy());
		DOCA_LOG_ERR("Failed to register data-ids argp param: %s", doca_error_get_name(status));
		return status;
	}

	status = register_argp_param(DOCA_ARGP_TYPE_STRING,
				     nullptr,
				     "rules",
				     "Path to JSON file containing rule definitions",
				     false,
				     [](void *value, void *cfg) noexcept {
					     static_cast<configuration *>(cfg)->rules_file_path =
						     static_cast<char const *>(value);
					     return DOCA_SUCCESS;
				     });
	if (status != DOCA_SUCCESS) {
		static_cast<void>(doca_argp_destroy());
		DOCA_LOG_ERR("Failed to register rules argp param: %s", doca_error_get_name(status));
		return status;
	}

	status = register_argp_param(DOCA_ARGP_TYPE_STRING,
				     nullptr,
				     "event-log",
				     "Path to output log file which will be populated with events information",
				     false,
				     [](void *value, void *cfg) noexcept {
					     static_cast<configuration *>(cfg)->event_log_file_path =
						     static_cast<char const *>(value);
					     return DOCA_SUCCESS;
				     });
	if (status != DOCA_SUCCESS) {
		static_cast<void>(doca_argp_destroy());
		DOCA_LOG_ERR("Failed to register event-log argp param: %s", doca_error_get_name(status));
		return status;
	}

	status = register_argp_param(DOCA_ARGP_TYPE_BOOLEAN,
				     nullptr,
				     "force-ownership",
				     "Forcefully acquire ownership of diag",
				     false,
				     [](void *value, void *cfg) noexcept {
					     static_cast<configuration *>(cfg)->force_diag_ownership =
						     *static_cast<bool const *>(value);
					     return DOCA_SUCCESS;
				     });
	if (status != DOCA_SUCCESS) {
		static_cast<void>(doca_argp_destroy());
		DOCA_LOG_ERR("Failed to register force-ownership argp param: %s", doca_error_get_name(status));
		return status;
	}

	status = doca_argp_start(argc, argv);
	if (status != DOCA_SUCCESS) {
		static_cast<void>(doca_argp_destroy());
		DOCA_LOG_ERR("Failed to parse CLI args: %s", doca_error_get_name(status));
		return status;
	}

	static_cast<void>(doca_argp_destroy());

	return DOCA_SUCCESS;
}

doca_error_t load_file_into_buffer(std::string const &path, std::vector<char> &file_data) noexcept
{
	std::ifstream file{path, std::ios::binary | std::ios::ate};
	if (!file) {
		DOCA_LOG_ERR("Unable to open file: \"%s\"", path.c_str());
		return DOCA_ERROR_NOT_FOUND;
	}

	auto const data_size = file.tellg();
	if (data_size <= 0) {
		DOCA_LOG_ERR("File: \"%s\" is empty", path.c_str());
		return DOCA_ERROR_INVALID_VALUE;
	}

	file_data.clear();
	file_data.resize(data_size);

	file.seekg(0);
	if (!file.read(&file_data[0], file_data.size())) {
		DOCA_LOG_ERR("Failed to read content of file: \"%s\"", path.c_str());
		return DOCA_ERROR_IO_FAILED;
	}

	/* Ensure data ends with null character */
	if (*file_data.rbegin() != '\0') {
		file_data.push_back('\0');
	}

	return DOCA_SUCCESS;
}

doca_error_t parse_json_configuration(char const *json_str,
				      uint32_t &diag_sample_period_ns_out,
				      uint32_t &sample_history_depth_out,
				      std::vector<data_id_definition> &data_id_definitions_out,
				      std::vector<rule_definition> &rule_definitions_out) noexcept
{
	json_object *json_root;
	json_object *json_node;
	doca_error_t status;
	json_root = json_tokener_parse(json_str);
	if (json_root == nullptr) {
		DOCA_LOG_ERR("Failed to parse rules data as json");
		return DOCA_ERROR_INVALID_VALUE;
	}

	std::unique_ptr<json_object, void (*)(json_object *)> release_json{json_root, [](json_object *obj) {
										   json_object_put(obj);
									   }};

	if (!json_object_object_get_ex(json_root, "diag_sample_period_ns", &json_node)) {
		DOCA_LOG_ERR("Missing \"diag_sample_period_ns\" node in the JSON data");
		return DOCA_ERROR_INVALID_VALUE;
	}
	diag_sample_period_ns_out = std::strtoul(json_object_get_string(json_node), nullptr, 10);
	DOCA_LOG_DBG("diag_sample_period_ns: %u", diag_sample_period_ns_out);

	if (!json_object_object_get_ex(json_root, "sample_history_depth", &json_node)) {
		DOCA_LOG_ERR("Missing \"sample_history_depth\" node in the JSON data");
		return DOCA_ERROR_INVALID_VALUE;
	}
	sample_history_depth_out = std::strtoul(json_object_get_string(json_node), nullptr, 10);
	DOCA_LOG_DBG("sample_history_depth: %u", sample_history_depth_out);

	if (!json_object_object_get_ex(json_root, "data-ids", &json_node)) {
		DOCA_LOG_ERR("Missing \"data-ids\" node in the JSON data");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (!json_object_is_type(json_node, json_type_array)) {
		DOCA_LOG_ERR("Malformed JSON data \"data-ids\" is not an array");
		return DOCA_ERROR_INVALID_VALUE;
	}

	size_t const num_data_ids = json_object_array_length(json_node);
	if (num_data_ids == 0) {
		DOCA_LOG_ERR("The \"data-ids\" array in the JSON data is empty");
		return DOCA_ERROR_INVALID_VALUE;
	}

	DOCA_LOG_DBG("Parsing %zu data-ids", num_data_ids);
	data_id_definitions_out.reserve(num_data_ids);
	for (size_t ii = 0; ii != num_data_ids; ++ii) {
		status = parse_json_data_id(json_object_array_get_idx(json_node, ii), data_id_definitions_out);
		if (status != DOCA_SUCCESS)
			return status;
	}

	if (!json_object_object_get_ex(json_root, "rules", &json_node)) {
		DOCA_LOG_ERR("Missing \"rules\" node in the JSON data");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (!json_object_is_type(json_node, json_type_array)) {
		DOCA_LOG_ERR("Malformed JSON data \"rules\" is not an array");
		return DOCA_ERROR_INVALID_VALUE;
	}

	size_t const num_rules = json_object_array_length(json_node);
	if (num_rules == 0) {
		DOCA_LOG_ERR("The \"rules\" array in the JSON data is empty");
		return DOCA_ERROR_INVALID_VALUE;
	}

	DOCA_LOG_DBG("Parsing %zu rules", num_rules);
	rule_definitions_out.reserve(num_rules);
	for (size_t ii = 0; ii != num_rules; ++ii) {
		status = parse_json_rule_data(json_object_array_get_idx(json_node, ii), rule_definitions_out);
		if (status != DOCA_SUCCESS)
			return status;
	}

	return DOCA_SUCCESS;
}

bool validate_configuration(configuration const &cfg)
{
	/* Verify history depth */
	if (!is_power_of_two(cfg.sample_history_depth)) {
		DOCA_LOG_ERR("sample_history_depth must be a power of two");
		return false;
	}

	/* Verify that the data ID used by each rule is defined in the data ID list */
	for (auto const &rule_def : cfg.rule_definitions) {
		auto const match = std::find_if(std::begin(cfg.data_id_definitions),
						std::end(cfg.data_id_definitions),
						[&rule_def](data_id_definition const &data_id) {
							return data_id.data_id == rule_def.data_id;
						});
		if (match == std::end(cfg.data_id_definitions)) {
			DOCA_LOG_ERR("Rule: \"%s\" data id: 0x%0llx not found in %s",
				     rule_def.name.c_str(),
				     static_cast<unsigned long long>(rule_def.data_id),
				     cfg.data_ids_file_path.c_str());
			return false;
		}
	}

	return true;
}

samples_circular_buffer::~samples_circular_buffer() = default;

samples_circular_buffer::samples_circular_buffer()
	: m_memory{},
	  m_samples{nullptr},
	  m_num_values_per_sample{0},
	  m_head_idx{0},
	  m_num_stored{0},
	  m_wrap_mask{0}
{
}

doca_error_t samples_circular_buffer::init_data_aligned_ring(uint32_t alignment,
							     uint32_t num_values_per_sample,
							     uint32_t sample_history_depth)
{
	if (alignment % sizeof(uint64_t) != 0) {
		DOCA_LOG_ERR("alignment must be a multiple of the size of a uint64_t");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (num_values_per_sample < 2) {
		/* timestamp + one data value */
		DOCA_LOG_ERR("num_values_per_sample must be at least 2");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (!is_power_of_two(sample_history_depth)) {
		DOCA_LOG_ERR("sample_history_depth must be a power of two");
		return DOCA_ERROR_INVALID_VALUE;
	}

	size_t const unaligned_size = sizeof(uint64_t) * num_values_per_sample * sample_history_depth;
	size_t aligned_size = alignment + unaligned_size;
	std::unique_ptr<uint64_t[]> new_memory{new uint64_t[aligned_size / sizeof(uint64_t)]};

	void *data = new_memory.get();
	data = std::align(alignment, unaligned_size, data, aligned_size);
	if (data == nullptr) {
		DOCA_LOG_ERR("Failed to align memory");
		return DOCA_ERROR_OPERATING_SYSTEM;
	}

	m_wrap_mask = sample_history_depth - 1;
	m_head_idx = m_wrap_mask;
	std::swap(m_memory, new_memory);
	m_samples = static_cast<uint64_t *>(data);
	m_num_values_per_sample = num_values_per_sample;
	m_num_stored = 0;

	::memset(m_samples, 0, unaligned_size);

	return DOCA_SUCCESS;
}

rule::~rule() = default;
rule::rule() = default;
rule::rule(rule &&) noexcept = default;
rule &rule::operator=(rule &&) noexcept = default;

doca_error_t rule::configure(rule_comparison_type comparison_type,
			     uint64_t comparison_value,
			     uint16_t sample_column_idx,
			     uint32_t hysteresis_threshold) noexcept
{
	m_comparison_value = comparison_value;
	m_sample_data_idx = sample_column_idx;
	m_hysteresis_threshold = hysteresis_threshold;

	switch (comparison_type) {
	case rule_comparison_type::incremental:
		m_comp = inc_comparator;
		/* Trigger immediately, does, not wait for hysteresis_threshold more samples*/
		m_hysteresis_threshold = 1;
		break;
	case rule_comparison_type::differential_greater_than:
		m_comp = dt_gt_comparator;
		break;
	case rule_comparison_type::differential_less_than:
		m_comp = dt_lt_comparator;
		break;
	case rule_comparison_type::greater_than:
		m_comp = gt_comparator;
		break;
	case rule_comparison_type::less_than:
		m_comp = lt_comparator;
		break;
	default:
		DOCA_LOG_ERR("[BUG] Unhandled comparator type: %u", static_cast<uint32_t>(comparison_type));
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

void log_app_start_header(std::ostream &os, configuration const &cfg)
{
	auto const prev_fill = os.fill();

	os << std::setfill('0');
	os << "Telemetry monitor : " << DOCA_VERSION_STRING << "\n";
	os << "Configuration: \n";
	os << "\tDevice: \"" << cfg.device_id << "\"\n";
	os << "\tSample history depth: " << cfg.sample_history_depth << "\n";
	os << "\tDiag sample period: " << cfg.diag_sample_period_ns << "ns\n";
	os << "\tData IDs: [\n";
	for (auto const &data_id_def : cfg.data_id_definitions) {
		os << "\t\t{\n";
		os << "\t\t\tName: \"" << data_id_def.name << "\",\n";
		os << "\t\t\tData ID: 0x" << std::hex << std::setw(16) << data_id_def.data_id << std::dec << "\n";
		os << "\t\t},\n";
	}
	os << "\t]\n";

	os << "\tRules: [\n";
	for (auto const &rule_def : cfg.rule_definitions) {
		os << "\t\t{\n";
		os << "\t\t\tName: \"" << rule_def.name << "\",\n";
		os << "\t\t\tData ID: 0x" << std::hex << std::setw(16) << rule_def.data_id << std::dec << "\n";
		os << "\t\t\tComparison type: " << rule_def.comparison_type << "\n";
		os << "\t\t\tComparison value: " << rule_def.comparison_value << "\n";
		os << "\t\t},\n";
	}
	os << "\t]\n\n\n";

	static_cast<void>(os.fill(prev_fill));
}

void log_data_id_names(std::ostream &os, std::vector<data_id_definition> const &data_ids)
{
	os << "timestamp";
	for (auto const &data_id : data_ids) {
		os << ", " << data_id.name;
	}
	os << '\n';
}

void log_event_started(std::ostream &os, rule_definition const &rule_def)
{
	os << "<<Event start>> Time: " << date_string(std::chrono::system_clock::now()) << ", Rule: \"" << rule_def.name
	   << "\"\n";
}

void log_event_completed(std::ostream &os, rule_definition const &rule_def)
{
	os << "<<Event end>> Time: " << date_string(std::chrono::system_clock::now()) << ", Rule: \"" << rule_def.name
	   << "\"\n";
}

void log_event_sample(std::ostream &os, uint64_t const *sample, uint32_t const num_values_per_sample)
{
	/* A sample is a timestamp followed by {num_values_per_sample} values */
	os << *sample;
	++sample;
	for (uint32_t jj = 0; jj != num_values_per_sample; ++jj) {
		os << ", " << *sample;
		++sample;
	}
	os << '\n';
}

void log_event_samples(std::ostream &os,
		       samples_circular_buffer const &cb,
		       uint32_t const num_values_per_sample,
		       uint32_t num_samples_to_log)
{
	uint32_t sample_idx = cb.get_previous_idx(num_samples_to_log - 1);

	for (uint32_t ii = 0; ii != num_samples_to_log; ++ii) {
		uint64_t const *sample = cb.get_sample(sample_idx++);
		log_event_sample(os, sample, num_values_per_sample);
	}
}

void close_doca_dev(doca_dev *dev) noexcept
{
	auto const result = doca_dev_close(dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close DOCA device: %s", doca_error_get_name(result));
	}
}

doca_error_t open_doca_dev(std::string const &device_identifier,
			   std::unique_ptr<doca_dev, void (*)(doca_dev *)> &device) noexcept
{
	static auto constexpr pci_addr_len = sizeof("XX:XX.X") - sizeof('\0');
	static auto constexpr pci_long_addr_len = sizeof("XXXX:XX:XX.X") - sizeof('\0');
	static auto constexpr max_device_id_length =
		std::max(DOCA_DEVINFO_IFACE_NAME_SIZE, DOCA_DEVINFO_IFACE_NAME_SIZE);

	doca_error_t status;
	doca_devinfo **list = nullptr;
	uint32_t list_size = 0;

	status = doca_devinfo_create_list(&list, &list_size);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to enumerate doca devices: %s", doca_error_get_name(status));
		return status;
	}

	doca_devinfo *selected_devinfo = nullptr;

	for (uint32_t ii = 0; ii != list_size; ++ii) {
		auto *devinfo = list[ii];
		std::array<char, max_device_id_length> device_id;

		if (device_identifier.size() == pci_addr_len || device_identifier.size() == pci_long_addr_len) {
			uint8_t is_addr_equal = 0;
			status = doca_devinfo_is_equal_pci_addr(devinfo, device_identifier.c_str(), &is_addr_equal);
			if (status == DOCA_SUCCESS && is_addr_equal) {
				selected_devinfo = devinfo;
				break;
			}
		}

		status = doca_devinfo_get_ibdev_name(devinfo, device_id.data(), device_id.size());
		if (status == DOCA_SUCCESS) {
			if (strcmp(device_identifier.c_str(), device_id.data()) == 0) {
				selected_devinfo = devinfo;
				break;
			}
		}

		status = doca_devinfo_get_iface_name(devinfo, device_id.data(), device_id.size());
		if (status == DOCA_SUCCESS) {
			if (strcmp(device_identifier.c_str(), device_id.data()) == 0) {
				selected_devinfo = devinfo;
				break;
			}
		}
	};

	doca_dev *opened_device;
	if (selected_devinfo != nullptr) {
		status = doca_dev_open(selected_devinfo, &opened_device);
		if (status == DOCA_SUCCESS) {
			device.reset(opened_device);
		} else {
			DOCA_LOG_ERR("Failed to open DOCA device: %s", doca_error_get_name(status));
		}
	} else {
		DOCA_LOG_ERR("No DOCA device found that matched given identifier: \"%s\"", device_identifier.c_str());
		status = DOCA_ERROR_NOT_FOUND;
	}

	static_cast<void>(doca_devinfo_destroy_list(list));

	return status;
}

doca_error_t check_dev_supports_doca_telemetry_diag(configuration const &cfg, doca_dev *dev) noexcept
{
	doca_error_t status;
	doca_devinfo const *const devinfo = doca_dev_as_devinfo(dev);

	/* It supports the doca_telemetry_diag feature */
	status = doca_telemetry_diag_cap_is_supported(devinfo);
	if (status != DOCA_SUCCESS) {
		if (status == DOCA_ERROR_NOT_SUPPORTED) {
			DOCA_LOG_ERR("Device does not support doca_telemetry_diag");
		} else {
			DOCA_LOG_ERR("doca_telemetry_diag_cap_is_supported failed: %s", doca_error_get_name(status));
		}
		return status;
	}

	/* It can support the number of data_id values the user specified */
	uint32_t max_num_data_ids = 0;
	status = doca_telemetry_diag_cap_get_max_num_data_ids(devinfo, &max_num_data_ids);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("doca_telemetry_diag_cap_get_max_num_data_ids failed: %s", doca_error_get_name(status));
		return status;
	}

	if (cfg.data_id_definitions.size() > max_num_data_ids) {
		DOCA_LOG_ERR("Cannot use %zu data-ids as specified in %s. Device can only support up to %u data-ids",
			     cfg.data_id_definitions.size(),
			     cfg.data_ids_file_path.c_str(),
			     max_num_data_ids);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	/* It supports repetitive sampling mode */
	uint8_t sample_mode_supported = 0;
	status = doca_telemetry_diag_cap_is_sample_mode_supported(devinfo,
								  DOCA_TELEMETRY_DIAG_SAMPLE_MODE_REPETITIVE,
								  &sample_mode_supported);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("doca_telemetry_diag_cap_is_sample_mode_supported failed: %s",
			     doca_error_get_name(status));
		return status;
	}

	if (sample_mode_supported == 0) {
		DOCA_LOG_ERR("DOCA_TELEMETRY_DIAG_SAMPLE_MODE_REPETITIVE is not supported");
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	/* It supports RTC timestamps */
	uint8_t timestamp_source_supported = 0;
	status = doca_telemetry_diag_cap_is_data_timestamp_source_supported(devinfo,
									    DOCA_TELEMETRY_DIAG_TIMESTAMP_SOURCE_RTC,
									    &timestamp_source_supported);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("doca_telemetry_diag_cap_is_data_timestamp_source_supported failed: %s",
			     doca_error_get_name(status));
		return status;
	}

	if (timestamp_source_supported == 0) {
		DOCA_LOG_ERR("DOCA_TELEMETRY_DIAG_TIMESTAMP_SOURCE_RTC is not supported");
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	return DOCA_SUCCESS;
}

void stop_and_destroy_diag_ctx(doca_telemetry_diag *diag) noexcept
{
	doca_error_t status;
	status = doca_telemetry_diag_stop(diag);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop telemetry_diag: %s", doca_error_get_name(status));
	}

	status = doca_telemetry_diag_destroy(diag);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy telemetry_diag: %s", doca_error_get_name(status));
	}
}

doca_error_t create_and_prepare_diag_ctx(configuration const &cfg,
					 doca_dev *dev,
					 std::unique_ptr<doca_telemetry_diag, void (*)(doca_telemetry_diag *)> &diag)
{
	doca_error_t status;

	if (!is_power_of_two(cfg.sample_history_depth)) {
		DOCA_LOG_ERR("Diag context creation failed: sample history depth must be a power of two");
		return DOCA_ERROR_INVALID_VALUE;
	}

	uint8_t const log_history_buffer_len = uint_to_log_2(cfg.sample_history_depth);
	uint8_t log_max_num_samples = 0;
	status = doca_telemetry_diag_cap_get_log_max_num_samples(doca_dev_as_devinfo(dev), &log_max_num_samples);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("doca_telemetry_diag_cap_get_log_max_num_samples failed: %s", doca_error_get_name(status));
		return status;
	}

	std::unique_ptr<doca_telemetry_diag, void (*)(doca_telemetry_diag *)> tmp_diag{nullptr,
										       stop_and_destroy_diag_ctx};
	{
		doca_telemetry_diag *diag_ptr = nullptr;

		status = doca_telemetry_diag_create(dev, cfg.force_diag_ownership, &diag_ptr);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create doca_telemetry_diag context: %s", doca_error_get_name(status));
			return status;
		}

		tmp_diag.reset(diag_ptr);
	}

	status = doca_telemetry_diag_set_output_format(tmp_diag.get(), DOCA_TELEMETRY_DIAG_OUTPUT_FORMAT_1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("doca_telemetry_diag_set_output_format failed: %s", doca_error_get_name(status));
		return status;
	}

	status = doca_telemetry_diag_set_sample_period(tmp_diag.get(), cfg.diag_sample_period_ns);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("doca_telemetry_diag_set_sample_period failed: %s", doca_error_get_name(status));
		return status;
	}

	status = doca_telemetry_diag_set_log_max_num_samples(tmp_diag.get(),
							     std::min(log_history_buffer_len, log_max_num_samples));
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("doca_telemetry_diag_set_log_max_num_samples failed: %s", doca_error_get_name(status));
		return status;
	}

	status = doca_telemetry_diag_set_max_num_data_ids(tmp_diag.get(), cfg.data_id_definitions.size());
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("doca_telemetry_diag_set_max_num_data_ids failed: %s", doca_error_get_name(status));
		return status;
	}

	status = doca_telemetry_diag_set_sync_mode(tmp_diag.get(), DOCA_TELEMETRY_DIAG_SYNC_MODE_NO_SYNC);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("doca_telemetry_diag_set_sync_mode failed: %s", doca_error_get_name(status));
		return status;
	}

	status = doca_telemetry_diag_set_sample_mode(tmp_diag.get(), DOCA_TELEMETRY_DIAG_SAMPLE_MODE_REPETITIVE);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("doca_telemetry_diag_set_sample_mode failed: %s", doca_error_get_name(status));
		return status;
	}

	uint8_t constexpr dont_clear_exiting_data = 0;
	status = doca_telemetry_diag_set_data_clear(tmp_diag.get(), dont_clear_exiting_data);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("doca_telemetry_diag_set_data_clear failed: %s", doca_error_get_name(status));
		return status;
	}

	status =
		doca_telemetry_diag_set_data_timestamp_source(tmp_diag.get(), DOCA_TELEMETRY_DIAG_TIMESTAMP_SOURCE_RTC);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("doca_telemetry_diag_set_data_timestamp_source failed: %s", doca_error_get_name(status));
		return status;
	}

	status = doca_telemetry_diag_apply_config(tmp_diag.get());
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("doca_telemetry_diag_apply_config failed: %s", doca_error_get_name(status));
		return status;
	}

	std::vector<uint64_t> data_ids;
	data_ids.reserve(cfg.data_id_definitions.size());
	std::transform(std::begin(cfg.data_id_definitions),
		       std::end(cfg.data_id_definitions),
		       std::back_inserter(data_ids),
		       [](auto const &data_id_def) {
			       return data_id_def.data_id;
		       });

	uint64_t counter_id_failure = 0;
	status = doca_telemetry_diag_apply_counters_list_by_id(tmp_diag.get(),
							       data_ids.data(),
							       data_ids.size(),
							       &counter_id_failure);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("doca_telemetry_diag_apply_counters_list_by_id failed: %s. Failed data_id: %lu",
			     doca_error_get_name(status),
			     counter_id_failure);
		return status;
	}

	/* Finally provide the now initialized context to the caller */
	diag.reset(tmp_diag.release());

	return DOCA_SUCCESS;
}
