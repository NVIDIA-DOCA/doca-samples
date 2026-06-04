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

#ifndef TRACEBACK_CORE_HPP_
#define TRACEBACK_CORE_HPP_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <doca_dev.h>
#include <doca_error.h>
#include <doca_telemetry_diag.h>

uint32_t constexpr DESIRED_MEMORY_ALIGNMENT = 64;

enum class rule_comparison_type {
	incremental,
	differential_greater_than,
	differential_less_than,
	greater_than,
	less_than
};

std::ostream &operator<<(std::ostream &os, rule_comparison_type comparison_type);

struct rule_definition {
	std::string name;
	uint64_t data_id;
	rule_comparison_type comparison_type;
	uint64_t comparison_value;
};

struct data_id_definition {
	std::string name;
	uint64_t data_id;
};

struct configuration {
	std::string device_id;
	std::string rules_file_path;
	std::string data_ids_file_path;
	std::string event_log_file_path;
	uint8_t force_diag_ownership;

	uint32_t diag_sample_period_ns;
	uint32_t sample_history_depth;
	std::vector<data_id_definition> data_id_definitions;
	std::vector<rule_definition> rule_definitions;
};

/**
 * A circular buffer to hold a collection of samples from the doca_telemetry_diag instance. This data is interrogated
 * by the rules to determine if an event is active or not. When an event triggers and while any event is active this
 * buffer also contains the data that will be written to the logs.
 */
class samples_circular_buffer {
public:
	~samples_circular_buffer();
	samples_circular_buffer();
	samples_circular_buffer(samples_circular_buffer const &) = delete;
	samples_circular_buffer(samples_circular_buffer &&) noexcept = delete;
	samples_circular_buffer &operator=(samples_circular_buffer const &) = delete;
	samples_circular_buffer &operator=(samples_circular_buffer &&) noexcept = delete;

	/**
	 * Initialize a circular buffer. Allocating an aligned internal memory buffer big enough to hold
	 * {sample_history_depth} samples. Where a sample is {num_values_per_sample} uint64_t values.
	 * @alignment [in]: Alignment to use when allocating the memory buffer.
	 * @num_values_per_sample [in]: Number of 64byte values required per sample.
	 * @sample_history_depth [in]: Number of samples to be held by this buffer.
	 * @return DOCA_SUCCESS upon success or a suitable error status code upon failure.
	 */
	doca_error_t init_data_aligned_ring(uint32_t alignment,
					    uint32_t num_values_per_sample,
					    uint32_t sample_history_depth);

	/**
	 * Get the Nth sample from the buffer (mutable).
	 * @idx [in]: Index to access (value is wrapped to protected from out of bounds access).
	 * @return Pointer to array of 64 byte values where the zeroth element is the timestamp.
	 */
	inline uint64_t *get_sample(uint32_t idx) noexcept
	{
		return m_samples + ((idx & m_wrap_mask) * m_num_values_per_sample);
	}

	/**
	 * Get the Nth sample from the buffer (immutable).
	 * @idx [in]: Index to access (value is wrapped to protected from out of bounds access).
	 * @return Pointer to array of 64 byte values where the zeroth element is the timestamp.
	 */
	inline uint64_t const *get_sample(uint32_t idx) const noexcept
	{
		return m_samples + ((idx & m_wrap_mask) * m_num_values_per_sample);
	}

	/**
	 * Get the index to the head (most recently) populated sample. This function only returns a valid value when the
	 * buffer contains at least one sample.
	 * @return Index of the head (most recent) value.
	 */
	inline uint32_t get_head_idx() const noexcept
	{
		return m_head_idx;
	}

	/**
	 * Get a past index (relative to head) where a distance of 0 would return the head index. This function only
	 * returns a valid value when the buffer contains at least one sample.
	 * @distance [in]: Distance (from head) to travel in reverse.
	 * @return unwrapped past index value. using this value with `get_sample` is fine because `get_sample` will wrap
	 * it into the correct bounds.
	 */
	inline uint32_t get_previous_idx(uint32_t distance) const noexcept
	{
		return m_head_idx - distance;
	}

	/**
	 * Move the head index forward.
	 */
	inline void advance_head_idx() noexcept
	{
		++m_head_idx;
		m_head_idx &= m_wrap_mask;
		if (m_num_stored <= m_wrap_mask)
			++m_num_stored;
	}

	/**
	 * Return the number of samples held in the buffer. Always returns a value in the range of:
	 * [0,{sample_history_depth}].
	 * @return Number of stored.
	 */
	uint32_t get_num_stored() const noexcept
	{
		return m_num_stored;
	}

	/**
	 * Reduce the number of stored samples by {num_samples}. Never reduces num stored below 0. This semantically
	 * erases samples from the buffer, but they are simply ignored and not erased or set to 0 to save cpu cycles.
	 * @num_samples [in]: Number of samples to erase.
	 */
	void erase(uint32_t num_samples) noexcept
	{
		if (num_samples < m_num_stored)
			m_num_stored -= num_samples;
		else
			m_num_stored = 0;
	}

private:
	std::unique_ptr<uint64_t[]> m_memory;
	uint64_t *m_samples;
	uint32_t m_num_values_per_sample;
	uint32_t m_head_idx;
	uint32_t m_num_stored;
	uint32_t m_wrap_mask;
};

/**
 * A rule object which will monitor the value returned by a single data-id and trigger an event when it crossed the
 * limit defined by the user.
 */
struct rule {
public:
	~rule();
	rule();
	rule(rule const &) = delete;
	rule(rule &&) noexcept;
	rule &operator=(rule const &) = delete;
	rule &operator=(rule &&) noexcept;

	/**
	 * Configure this rule.
	 * @comparison_type [in]: Which type of comparison this rule should use.
	 * @comparison_value[in]: The value this rule should use when monitoring its assigned data-id
	 * @param sample_record_offset[in]: Offset into the sample record which this rule should monitor.
	 * @param hysteresis_threshold[in]: Number of samples of history to consider when transitioning the rule from
	 * active to inactive.
	 * @return DOCA_SUCCESS upon success or a suitable error status code upon failure.
	 */
	doca_error_t configure(rule_comparison_type comparison_type,
			       uint64_t comparison_value,
			       uint16_t sample_record_offset,
			       uint32_t hysteresis_threshold) noexcept;

	/**
	 * Process a sample record.
	 * @buffer [in]: Circular buffer holding the sample records data to be evaluated.
	 * @return true upon state change, false otherwise.
	 */
	bool process_sample(samples_circular_buffer const &buffer) noexcept
	{
		m_comp(*this, buffer);

		if (m_state_transition_counter >= m_hysteresis_threshold) {
			m_is_active = !m_is_active;
			m_state_transition_counter = 0;
			return true;
		}

		return false;
	}

	/**
	 * Get the rule state.
	 * @return true if the rule is active, false otherwise.
	 */
	bool is_active() const noexcept
	{
		return m_is_active;
	}

private:
	using comparator_t = void (*)(rule &self, samples_circular_buffer const &buffer) noexcept;

	comparator_t m_comp = nullptr;
	uint64_t m_incremental_target_value = 0;
	uint64_t m_comparison_value = 0;
	uint32_t m_state_transition_counter = 0;
	uint32_t m_hysteresis_threshold = 0;
	uint16_t m_sample_data_idx = 0;
	bool m_is_active = false;
	bool m_set_new_target_value = true;

	/*
	 * Incremental comparison activates the rule immediately once any sample exceeds the threshold. The comparator
	 * will calculate a new threshold each time a sample exceeds it's limit. The rule therefore stays active until a
	 * sample less than the newly calculated threshold is received when it will deactivate. This means that a rule
	 * will trigger at least once each time the sampled value increases by {hysteresis_threshold}.
	 */

	static void inc_comparator(rule &self, samples_circular_buffer const &buffer) noexcept
	{
		if (self.m_set_new_target_value) {
			/* If the buffer only holds one sample the new threshold must be relative to that sample,
			 * otherwise it should be relative to the previous sample which will be the sample that exceeded
			 * the current threshold.
			 */
			uint32_t const relative_sample_idx = buffer.get_num_stored() == 1 ? buffer.get_head_idx() :
											    buffer.get_previous_idx(1);
			self.m_incremental_target_value =
				buffer.get_sample(relative_sample_idx)[self.m_sample_data_idx] +
				self.m_comparison_value;
			self.m_set_new_target_value = false;
		}

		auto const comp_match = buffer.get_sample(buffer.get_head_idx())[self.m_sample_data_idx] >=
					self.m_incremental_target_value;

		/* If the value exceeds the current threshold a new threshold must be calculated */
		if (comp_match) {
			self.m_set_new_target_value = true;
		}

		/*
		 * When not active and the comparator matches OR when active and the comparator does not match
		 * count up to {hysteresis_threshold} at which point the rule will change m_ Otherwise,
		 * remain in the current state and reset the transition counter.
		 */
		if ((self.m_is_active && !comp_match) || (!self.m_is_active && comp_match)) {
			++self.m_state_transition_counter;
		} else {
			self.m_state_transition_counter = 0;
		}
	}

	/**
	 * Differential value greater than comparator. This comparator considers the differential between the most
	 * recent sample value and the value of that sample {hysteresis_threshold} samples in the past. If the
	 * difference between those values is greater than {comparison_value} the rule will be triggered. The rule will
	 * remain active until the differential has reduced to less than {comparison_value} for {hysteresis_threshold}
	 * consecutive samples
	 */
	static void dt_gt_comparator(rule &self, samples_circular_buffer const &buffer) noexcept
	{
		/* Buffer must contain {hysteresis_threshold} + the current sample to operator upon */
		if (buffer.get_num_stored() <= self.m_hysteresis_threshold)
			return;

		auto const cur_sample = buffer.get_sample(buffer.get_head_idx())[self.m_sample_data_idx];
		auto const past_sample =
			buffer.get_sample(buffer.get_previous_idx(self.m_hysteresis_threshold))[self.m_sample_data_idx];

		auto const comp_match = (cur_sample - past_sample) > self.m_comparison_value;

		if (!self.m_is_active && comp_match) {
			/* Activate immediately when differential rate is reached when not active */
			self.m_state_transition_counter = self.m_hysteresis_threshold;
		} else if (self.m_is_active && !comp_match) {
			/* Deactivate after {hysteresis_threshold} consecutive non-matches */
			++self.m_state_transition_counter;
		} else {
			/* Remain in current state and reset the transition counter.
			 * Matching when already active: stay active
			 * Not matching when not active: stay inactive
			 */
			self.m_state_transition_counter = 0;
		}
	}

	/**
	 * Differential value less than comparator. This comparator considers the differential between the most
	 * recent sample value and the value of that sample {hysteresis_threshold} samples in the past. If the
	 * difference between those values is less than {comparison_value} the rule will be triggered. The rule will
	 * remain active until the differential has increased to at least {comparison_value} for {hysteresis_threshold}
	 * consecutive samples.
	 *
	 * @rule [in]: Reference to self.
	 * @buffer [in]: Circular buffer holding sample data.
	 */
	static void dt_lt_comparator(rule &self, samples_circular_buffer const &buffer) noexcept
	{
		/* Buffer must contain {hysteresis_threshold} + the current sample to operator upon */
		if (buffer.get_num_stored() <= self.m_hysteresis_threshold)
			return;

		auto const cur_sample = buffer.get_sample(buffer.get_head_idx())[self.m_sample_data_idx];
		auto const prev_sample =
			buffer.get_sample(buffer.get_previous_idx(self.m_hysteresis_threshold))[self.m_sample_data_idx];

		auto const comp_match = (cur_sample - prev_sample) < self.m_comparison_value;

		if (!self.m_is_active && comp_match) {
			/* Activate immediately when differential rate is reached when not active */
			self.m_state_transition_counter = self.m_hysteresis_threshold;
		} else if (self.m_is_active && !comp_match) {
			/* Deactivate after {hysteresis_threshold} consecutive non-matches */
			++self.m_state_transition_counter;
		} else {
			/* Remain in current state and reset the transition counter.
			 * Matching when already active: stay active
			 * Not matching when not active: stay inactive
			 */
			self.m_state_transition_counter = 0;
		}
	}

	/**
	 * Value greater than comparator. This comparator considers the value of each sample. If {hysteresis_threshold}
	 * consecutive samples exceed {comparison_value} the rule will be triggered. The rule will remain active until
	 * {hysteresis_threshold} consecutive samples have a value of at most {comparison_value}.
	 *
	 * @rule [in]: Reference to self.
	 * @buffer [in]: Circular buffer holding sample data.
	 */
	static void gt_comparator(rule &self, samples_circular_buffer const &buffer) noexcept
	{
		auto const comp_match = buffer.get_sample(buffer.get_head_idx())[self.m_sample_data_idx] >
					self.m_comparison_value;
		/*
		 * When not active and the comparator matches OR when active and the comparator does not match
		 * count up to {hysteresis_threshold} at which point the rule will change m_ Otherwise,
		 * remain in the current state and reset the transition counter.
		 */
		if ((self.m_is_active && !comp_match) || (!self.m_is_active && comp_match)) {
			++self.m_state_transition_counter;
		} else {
			self.m_state_transition_counter = 0;
		}
	}

	/**
	 * Value less than comparator. This comparator considers the value of each sample. If {hysteresis_threshold}
	 * consecutive samples are less than {comparison_value} the rule will be triggered. The rule will remain active
	 * until {hysteresis_threshold} consecutive samples have a value of at least {comparison_value}.
	 *
	 * @rule [in]: Reference to self.
	 * @buffer [in]: Circular buffer holding sample data.
	 */
	static void lt_comparator(rule &self, samples_circular_buffer const &buffer) noexcept

	{
		auto const comp_match = buffer.get_sample(buffer.get_head_idx())[self.m_sample_data_idx] <
					self.m_comparison_value;
		/*
		 * When not active and the comparator matches OR when active and the comparator does not match
		 * count up to {hysteresis_threshold} at which point the rule will change m_ Otherwise,
		 * remain in the current state and reset the transition counter.
		 */
		if ((self.m_is_active && !comp_match) || (!self.m_is_active && comp_match)) {
			++self.m_state_transition_counter;
		} else {
			self.m_state_transition_counter = 0;
		}
	}
};

/**
 * Check if a value is a power of 2.
 * @value [in]: Value to check.
 * @return true if value is a power of two, false otherwise.
 */
inline bool is_power_of_two(uint64_t value)
{
	return value && (value & (value - uint64_t{1})) == 0;
}

/**
 * Parse application arguments from command line into a configuration structure.
 * @argc [in]: Number of arguments.
 * @argv [in]: Argument values.
 * @config [out]: Configuration structure to populate.
 * @return DOCA_SUCCESS upon success or a suitable error status code upon failure.
 */
doca_error_t parse_configuration(int argc, char **argv, configuration &config) noexcept;

/**
 * Load the content of a file into a character buffer.
 * @path [in]: Path to the file to load.
 * @file_data [out]: Vector to populate with the contents of the file.
 * @return DOCA_SUCCESS upon success or a suitable error status code upon failure.
 */
doca_error_t load_file_into_buffer(std::string const &path, std::vector<char> &file_data) noexcept;

/**
 * Parse json configuration.
 *
 * Expected format:
 * {
 *     "diag_sample_period_ns": 12345,
 *     "sample_history_depth": 512,
 *     "data-ids": [
 *         {
 *             "name": "port_0_rx_bytes",
 *             "data_id": "0x1020000100000000"
 *         }
 *     ],
 *     "rules": [
 *         {
 *             "name": "Rule 0",
 *             "data_id": "0x1020000100000000",
 *             "comparison_type": "differential_less_than",
 *             "comparison_value": 20000
 *         }
 *     ]
 * }
 * @json_str [in]: Json data to parse.
 * @diag_sample_period_ns_out [out]: Read value for diag_sample_period_ns.
 * @sample_history_depth_out [out]: Read value for sample_history_depth_out.
 * @data_id_definitions_out [out]: Parsed data id list.
 * @rule_definitions_out [out]: Parsed rule list.
 * @return DOCA_SUCCESS upon success or a suitable error status code upon failure.
 */
doca_error_t parse_json_configuration(char const *json_str,
				      uint32_t &diag_sample_period_ns_out,
				      uint32_t &sample_history_depth_out,
				      std::vector<data_id_definition> &data_id_definitions_out,
				      std::vector<rule_definition> &rule_definitions_out) noexcept;

/**
 * Validate configuration is valid and ready to use.
 * @cfg [in]: Fully populated configuration (Values from CLI args, files, or any other source is all applied).
 * @return DOCA_SUCCESS upon success or a suitable error status code upon failure.
 */
bool validate_configuration(configuration const &cfg);

/**
 * Log the fixed initial messages in the log file. This will contain:
 *  - doca version
 *  - application configuration
 * @os [in]: Stream to write data to.
 * @cfg [in]: Application configuration.
 */
void log_app_start_header(std::ostream &os, configuration const &cfg);

/**
 * Add a column names row so the user can see which data is in which column.
 *  - timestamp, data_id[0].name, data_id[N].name
 * @os [in]: Stream to write data to.
 * @data_ids [in]: List of data ID definitions.
 */
void log_data_id_names(std::ostream &os, std::vector<data_id_definition> const &data_ids);

/**
 * Log the activation of a rule
 * - <<Event start>> Time: {MACHINE_UTC_TIMESTAMP}, Rule: "{rule.name}"
 * @os [in]: Stream to write data to.
 * @rule_def [in]: Rule which has activated.
 */
void log_event_started(std::ostream &os, rule_definition const &rule_def);

/**
 * Log the deactivation of a rule
 * - <<Event end>> Time: {MACHINE_UTC_TIMESTAMP}, Rule: "{rule.name}"
 * @os [in]: Stream to write data to.
 * @rule_def [in]: Rule which has deactivated.
 */
void log_event_completed(std::ostream &os, rule_definition const &rule_def);

/**
 * Add a single sample data row to the log file:
 * - sample.timestamp, sample.data_id[0].value, sample.data_id[N].value
 * @os [in]: Stream to write data to.
 * @sample [in]: Sample to write.
 * @num_values_per_sample [in]: Number of data ids (after the time stamp) to write. Always equals data_ids.size().
 */
void log_event_sample(std::ostream &os, uint64_t const *sample, uint32_t num_values_per_sample);

/**
 * Log {num_samples_to_log} samples including the current sample:
 * - log_event_sample(sample[head-(num_samples_to_log - 1)])
 * ....
 * - log_event_sample(sample[head])
 * @os [in]: Stream to write data to.
 * @cb [in]: Circular buffer holding the samples.
 * @num_values_per_sample [in]: Number of data ids (after the time stamp) to write. Always equals data_ids.size().
 * @num_samples_to_log [in]: Number of samples to log.
 */
void log_event_samples(std::ostream &os,
		       samples_circular_buffer const &cb,
		       uint32_t num_values_per_sample,
		       uint32_t num_samples_to_log);

/**
 * Adapter that allows a doca_dev to be managed by a std::unique_ptr.
 * @dev [in]: Device to close.
 */
void close_doca_dev(doca_dev *dev) noexcept;

/**
 * Open a doca device for use by the application.
 * @device_identifier [in]: Identifier of the device to use.
 * @device [out]: Opened device.
 * @return DOCA_SUCCESS or an error status upon failure.
 */
doca_error_t open_doca_dev(std::string const &device_identifier,
			   std::unique_ptr<doca_dev, void (*)(doca_dev *)> &device) noexcept;

/**
 * Check that the selected device support the necessary doca_telemetry_diag capabilities.
 * @cfg [in]: Application configuration.
 * @dev [in]: Device to query.
 * @return DOCA_SUCCESS or an error status upon failure.
 */
doca_error_t check_dev_supports_doca_telemetry_diag(configuration const &cfg, doca_dev *dev) noexcept;

/**
 * Adapter to allow a doca_telemetry_diag to be destroyed by a std::unique_ptr.
 * @diag [in]: instance to destroy.
 */
void stop_and_destroy_diag_ctx(doca_telemetry_diag *diag) noexcept;

/**
 * Create, and configure a doca_telemetry_diag instance.
 * @cfg [in]: Application configuration.
 * @dev [in]: Device to use.
 * @diag [out]: Created doca_telemetry_diag instance.
 * @return DOCA_SUCCESS or an error status upon failure.
 */
doca_error_t create_and_prepare_diag_ctx(configuration const &cfg,
					 doca_dev *dev,
					 std::unique_ptr<doca_telemetry_diag, void (*)(doca_telemetry_diag *)> &diag);

#endif /* TRACEBACK_CORE_HPP_ */
