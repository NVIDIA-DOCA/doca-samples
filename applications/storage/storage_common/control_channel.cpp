/*
 * Copyright (c) 2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <storage_common/control_channel.hpp>

#include <chrono>
#include <thread>

#include <doca_comch.h>
#include <doca_log.h>
#include <doca_pe.h>

#include <storage_common/buffer_utils.hpp>
#include <storage_common/definitions.hpp>
#include <storage_common/doca_utils.hpp>
#include <storage_common/tcp_socket.hpp>

DOCA_LOG_REGISTER(CONTROL_CHANNEL);

using namespace std::string_literals;

namespace storage::control {

namespace {

template <typename InterfaceT>
class basic_control_channel : public InterfaceT {
public:
	basic_control_channel() : m_rx_buffer{}, m_rx_message{}
	{
		m_rx_buffer.reserve(1024 * 2);
	}
	basic_control_channel(basic_control_channel const &) = delete;
	basic_control_channel(basic_control_channel &&) noexcept = delete;
	basic_control_channel &operator=(basic_control_channel const &) = delete;
	basic_control_channel &operator=(basic_control_channel &&) noexcept = delete;

	void set_error(std::string msg)
	{
		m_error_message = std::move(msg);
	}

	void encode_message_to_tx_buffer(control::message const &message)
	{
		control::message_header const header{wire_size(message)};

		m_tx_buffer.resize(wire_size(header) + header.wire_size);

		static_cast<void>(encode(encode(m_tx_buffer.data(), header), message));
	}

	void append_rx_bytes(char const *bytes, size_t byte_count)
	{
		std::copy(bytes, bytes + byte_count, std::back_inserter(m_rx_buffer));
	}

	bool extract_message_from_rx_buffer()
	{
		if (m_rx_buffer.size() < sizeof(control::message_header))
			return false;

		control::message_header header{};

		auto *read_ptr = decode(m_rx_buffer.data(), header);

		if (m_rx_buffer.size() < (sizeof(control::message_header) + header.wire_size))
			return false;

		static_cast<void>(decode(read_ptr, m_rx_message));

		m_rx_buffer.erase(m_rx_buffer.begin(),
				  m_rx_buffer.begin() + sizeof(control::message_header) + header.wire_size);
		return true;
	}

protected:
	std::vector<char> m_tx_buffer;
	std::vector<char> m_rx_buffer;
	message m_rx_message;
	std::string m_error_message;
};

class tcp_control_channel : public basic_control_channel<storage::control::channel> {
public:
protected:
	void write_to_tcp_socket(storage::tcp_socket &socket, control::message const &msg)
	{
		encode_message_to_tx_buffer(msg);
		if (socket.write(m_tx_buffer.data(), m_tx_buffer.size()) != m_tx_buffer.size()) {
			throw storage::runtime_error{DOCA_ERROR_IO_FAILED, "Failed to send control message"};
		}
	}

	storage::control::message *poll_tcp_socket(storage::tcp_socket &socket)
	{
		std::array<char, 256> tmp_buff{};

		uint32_t const nb_read = socket.read(tmp_buff.data(), tmp_buff.size());
		if (nb_read != 0) {
			append_rx_bytes(tmp_buff.data(), nb_read);
		}

		if (extract_message_from_rx_buffer()) {
			return std::addressof(m_rx_message);
		}

		return nullptr;
	}
};

class comch_control_channel : public basic_control_channel<storage::control::comch_channel> {
public:
	~comch_control_channel() override
	{
		if (m_pe != nullptr) {
			auto const ret = doca_pe_destroy(m_pe);
			if (ret != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy doca_pe: %s", doca_error_get_name(ret));
			}
		}
	}

	comch_control_channel()
		: basic_control_channel<storage::control::comch_channel>{},
		  m_pe{nullptr},
		  m_connection{nullptr},
		  m_consumer_cb_data{.ptr = nullptr},
		  m_connection_cb{},
		  m_expiry_cb{}
	{
	}
	comch_control_channel(comch_control_channel const &) = delete;
	comch_control_channel(comch_control_channel &&) noexcept = delete;
	comch_control_channel &operator=(comch_control_channel const &) = delete;
	comch_control_channel &operator=(comch_control_channel &&) noexcept = delete;

	storage::control::message *poll() override
	{
		static_cast<void>(doca_pe_progress(m_pe));
		if (extract_message_from_rx_buffer()) {
			DOCA_LOG_DBG("%s", to_string(m_rx_message).c_str());
			return std::addressof(m_rx_message);
		}

		return nullptr;
	}

	doca_comch_connection *get_comch_connection() const noexcept override
	{
		return m_connection;
	}

protected:
	doca_pe *m_pe;
	doca_comch_connection *m_connection;
	doca_data m_consumer_cb_data;
	comch_channel::consumer_event_callback m_connection_cb;
	comch_channel::consumer_event_callback m_expiry_cb;

	comch_control_channel *get_self()
	{
		return this;
	}

	void create_pe()
	{
		auto const ret = doca_pe_create(&m_pe);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to create doca_pe"};
		}
	}

	void set_consumer_callbacks(void *callback_user_data,
				    comch_channel::consumer_event_callback new_consumer_event_cb,
				    comch_channel::consumer_event_callback expired_consumer_event_cb)
	{
		m_consumer_cb_data.ptr = callback_user_data;
		m_connection_cb = std::move(new_consumer_event_cb);
		m_expiry_cb = std::move(expired_consumer_event_cb);
	}

	static void new_consumer_event_cb(doca_comch_event_consumer *event,
					  doca_comch_connection *conn,
					  uint32_t id) noexcept
	{
		static_cast<void>(event);

		auto *self = static_cast<comch_control_channel *>(doca_comch_connection_get_user_data(conn).ptr);
		self->m_connection_cb(self->m_consumer_cb_data.ptr, id);
	}

	static void expired_consumer_event_cb(doca_comch_event_consumer *event,
					      doca_comch_connection *conn,
					      uint32_t id) noexcept
	{
		static_cast<void>(event);

		auto *self = static_cast<comch_control_channel *>(doca_comch_connection_get_user_data(conn).ptr);
		self->m_expiry_cb(self->m_consumer_cb_data.ptr, id);
	}

	/*
	 * ComCh control task send callback
	 *
	 * @task [in]: Completed task
	 * @task_user_data [in]: Data associated with the task
	 * @ctx_user_data [in]: Data associated with the context
	 */
	static void task_send_cb(doca_comch_task_send *task, doca_data task_user_data, doca_data ctx_user_data) noexcept
	{
		static_cast<void>(task_user_data);
		static_cast<void>(ctx_user_data);

		doca_task_free(doca_comch_task_send_as_task(task));
	}

	/*
	 * ComCh control task send error callback
	 *
	 * @task [in]: Failed task
	 * @task_user_data [in]: Data associated with the task
	 * @ctx_user_data [in]: Data associated with the context
	 */
	static void task_send_error_cb(doca_comch_task_send *task,
				       doca_data task_user_data,
				       doca_data ctx_user_data) noexcept
	{
		static_cast<void>(task_user_data);
		static_cast<comch_control_channel *>(ctx_user_data.ptr)
			->set_error("Failed to complete doca_comch_task_send");

		doca_task_free(doca_comch_task_send_as_task(task));
	}

	/*
	 * ComCh control message received callback
	 *
	 * @task [in]: Completed task
	 * @task_user_data [in]: Data associated with the task
	 * @ctx_user_data [in]: Data associated with the context
	 */
	static void event_msg_recv_cb(doca_comch_event_msg_recv *event,
				      uint8_t *recv_buffer,
				      uint32_t msg_len,
				      doca_comch_connection *comch_connection) noexcept
	{
		static_cast<void>(event);
		auto *const self =
			static_cast<comch_control_channel *>(doca_comch_connection_get_user_data(comch_connection).ptr);

		self->append_rx_bytes(reinterpret_cast<char const *>(recv_buffer), msg_len);
	}
};

class comch_client_control_channel : public comch_control_channel {
public:
	~comch_client_control_channel() override
	{
		cleanup();
	}
	comch_client_control_channel() = delete;
	comch_client_control_channel(doca_dev *dev,
				     char const *channel_name,
				     void *callback_user_data,
				     comch_channel::consumer_event_callback new_consumer_event_cb,
				     comch_channel::consumer_event_callback expired_consumer_event_cb)
		: comch_control_channel{},
		  m_comch_client{nullptr}
	{
		try {
			create_pe();
			set_consumer_callbacks(callback_user_data,
					       std::move(new_consumer_event_cb),
					       std::move(expired_consumer_event_cb));
			init(dev, channel_name);
		} catch (storage::runtime_error const &ex) {
			cleanup();
			throw;
		}
	}
	comch_client_control_channel(comch_client_control_channel const &) = delete;
	comch_client_control_channel(comch_client_control_channel &&) noexcept = delete;
	comch_client_control_channel &operator=(comch_client_control_channel const &) = delete;
	comch_client_control_channel &operator=(comch_client_control_channel &&) noexcept = delete;

	bool is_connected() override
	{
		static_cast<void>(doca_pe_progress(m_pe));

		if (!m_error_message.empty()) {
			throw storage::runtime_error{DOCA_ERROR_INITIALIZATION,
						     "Failed to connect to doca_comch client"};
		}

		doca_ctx_states cur_state;
		static_cast<void>(doca_ctx_get_state(doca_comch_client_as_ctx(m_comch_client), &cur_state));
		if (cur_state == DOCA_CTX_STATE_RUNNING) {
			auto const ret = doca_comch_client_get_connection(m_comch_client, &m_connection);
			if (ret != DOCA_SUCCESS) {
				throw std::runtime_error{"Failed to get comch client connection: "s +
							 doca_error_get_name(ret)};
			}

			static_cast<void>(
				doca_comch_connection_set_user_data(m_connection, doca_data{.ptr = get_self()}));

			DOCA_LOG_DBG("Connected to comch server");
			return true;
		}

		return false;
	}

	void send_message(message const &msg) override
	{
		doca_error_t ret;
		doca_comch_task_send *task;

		DOCA_LOG_DBG("%s", to_string(msg).c_str());
		encode_message_to_tx_buffer(msg);

		ret = doca_comch_client_task_send_alloc_init(m_comch_client,
							     m_connection,
							     m_tx_buffer.data(),
							     m_tx_buffer.size(),
							     &task);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to allocate comch task"};
		}

		ret = doca_task_submit(doca_comch_task_send_as_task(task));
		if (ret != DOCA_SUCCESS) {
			doca_task_free(doca_comch_task_send_as_task(task));
			throw storage::runtime_error{ret, "Failed to send control message"};
		}
	}

private:
	doca_comch_client *m_comch_client;

	void init(doca_dev *dev, char const *channel_name)
	{
		doca_error_t ret;

		ret = doca_comch_client_create(dev, channel_name, &m_comch_client);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to create doca_comch_client"};
		}

		ret = doca_comch_client_task_send_set_conf(m_comch_client,
							   comch_control_channel::task_send_cb,
							   comch_control_channel::task_send_error_cb,
							   storage::max_concurrent_control_messages);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to configure doca_comch_client send task pool"};
		}

		ret = doca_comch_client_event_msg_recv_register(m_comch_client,
								comch_control_channel::event_msg_recv_cb);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret,
						     "Failed to configure doca_comch_client receive task callback"};
		}

		ret = doca_comch_client_event_consumer_register(m_comch_client,
								comch_control_channel::new_consumer_event_cb,
								comch_control_channel::expired_consumer_event_cb);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{
				ret,
				"Failed to register for doca_comch_client consumer registration events"};
		}

		ret = doca_ctx_set_user_data(doca_comch_client_as_ctx(m_comch_client), doca_data{.ptr = get_self()});
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to set doca_comch_client user data"};
		}

		auto *comch_ctx = doca_comch_client_as_ctx(m_comch_client);

		ret = doca_pe_connect_ctx(m_pe, comch_ctx);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to connect doca_comch_client with doca_pe"};
		}

		ret = doca_ctx_start(doca_comch_client_as_ctx(m_comch_client));
		if (ret != DOCA_ERROR_IN_PROGRESS && ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to start doca_comch_client"};
		}
	}

	void cleanup() noexcept
	{
		doca_error_t ret;

		if (m_comch_client) {
			ret = storage::stop_context(doca_comch_client_as_ctx(m_comch_client), m_pe);
			if (ret != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to stop doca_comch_client: %s", doca_error_get_name(ret));
			}

			DOCA_LOG_DBG("Destroy doca_comch_client(%p)", m_comch_client);
			ret = doca_comch_client_destroy(m_comch_client);
			if (ret != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy doca_comch_client: %s", doca_error_get_name(ret));
			}
		}
	}
};

class comch_server_control_channel : public comch_control_channel {
public:
	~comch_server_control_channel() override
	{
		cleanup();
	}
	comch_server_control_channel() = delete;
	comch_server_control_channel(doca_dev *dev,
				     doca_dev_rep *dev_rep,
				     char const *channel_name,
				     void *callback_user_data,
				     comch_channel::consumer_event_callback new_consumer_event_cb,
				     comch_channel::consumer_event_callback expired_consumer_event_cb)
		: comch_control_channel{},
		  m_comch_server{nullptr}
	{
		try {
			create_pe();
			set_consumer_callbacks(callback_user_data,
					       std::move(new_consumer_event_cb),
					       std::move(expired_consumer_event_cb));
			init(dev, dev_rep, channel_name);
		} catch (storage::runtime_error const &ex) {
			cleanup();
			throw;
		}
	}

	comch_server_control_channel(comch_server_control_channel const &) = delete;
	comch_server_control_channel(comch_server_control_channel &&) noexcept = delete;
	comch_server_control_channel &operator=(comch_server_control_channel const &) = delete;
	comch_server_control_channel &operator=(comch_server_control_channel &&) noexcept = delete;

	bool is_connected() override
	{
		static_cast<void>(doca_pe_progress(m_pe));

		if (!m_error_message.empty()) {
			throw storage::runtime_error{DOCA_ERROR_INITIALIZATION,
						     "Failed to connect to doca_comch client"};
		}

		return m_connection != nullptr;
	}

	void send_message(message const &msg) override
	{
		doca_error_t ret;
		doca_comch_task_send *task;

		DOCA_LOG_DBG("%s", to_string(msg).c_str());
		encode_message_to_tx_buffer(msg);

		ret = doca_comch_server_task_send_alloc_init(m_comch_server,
							     m_connection,
							     m_tx_buffer.data(),
							     m_tx_buffer.size(),
							     &task);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to allocate comch task"};
		}

		ret = doca_task_submit(doca_comch_task_send_as_task(task));
		if (ret != DOCA_SUCCESS) {
			doca_task_free(doca_comch_task_send_as_task(task));
			throw storage::runtime_error{ret, "Failed to send control message"};
		}
	}

private:
	doca_comch_server *m_comch_server;

	void init(doca_dev *dev, doca_dev_rep *dev_rep, char const *channel_name)
	{
		doca_error_t ret;

		ret = doca_comch_server_create(dev, dev_rep, channel_name, &m_comch_server);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to create doca_comch_server"};
		}

		auto *comch_ctx = doca_comch_server_as_ctx(m_comch_server);

		ret = doca_pe_connect_ctx(m_pe, comch_ctx);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to connect doca_comch_server with doca_pe"};
		}

		ret = doca_comch_server_task_send_set_conf(m_comch_server,
							   comch_control_channel::task_send_cb,
							   comch_control_channel::task_send_error_cb,
							   storage::max_concurrent_control_messages);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to configure doca_comch_server send task pool"};
		}

		ret = doca_comch_server_event_msg_recv_register(m_comch_server,
								comch_control_channel::event_msg_recv_cb);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret,
						     "Failed to configure doca_comch_server receive task callback"};
		}

		ret = doca_comch_server_event_connection_status_changed_register(m_comch_server,
										 event_connection_connected_cb,
										 event_connection_disconnected_cb);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to configure doca_comch_server connection callbacks"};
		}

		ret = doca_comch_server_event_consumer_register(m_comch_server,
								comch_control_channel::new_consumer_event_cb,
								comch_control_channel::expired_consumer_event_cb);
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{
				ret,
				"Failed to register for doca_comch_server consumer registration events"};
		}

		ret = doca_ctx_set_user_data(comch_ctx, doca_data{.ptr = get_self()});
		if (ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret, "Failed to set doca_comch_server user data"};
		}

		ret = doca_ctx_start(comch_ctx);
		if (ret != DOCA_ERROR_IN_PROGRESS && ret != DOCA_SUCCESS) {
			throw storage::runtime_error{ret,
						     "[application::application] Failed to start doca_comch_server"};
		}
	}

	void cleanup() noexcept
	{
		doca_error_t ret;

		if (m_comch_server != nullptr) {
			ret = storage::stop_context(doca_comch_server_as_ctx(m_comch_server), m_pe);
			if (ret != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to stop doca_comch_server: %s", doca_error_get_name(ret));
			}

			ret = doca_comch_server_destroy(m_comch_server);
			if (ret != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy doca_comch_server: %s", doca_error_get_name(ret));
			}
		}
	}

	static void event_connection_connected_cb(doca_comch_event_connection_status_changed *event,
						  doca_comch_connection *conn,
						  uint8_t change_successful) noexcept
	{
		static_cast<void>(event);
		DOCA_LOG_DBG("Connection %p %s", conn, (change_successful ? "connected" : "refused"));

		if (change_successful == 0) {
			DOCA_LOG_ERR("Failed to accept new client connection");
			return;
		}

		doca_data user_data{.ptr = nullptr};
		auto const ret =
			doca_ctx_get_user_data(doca_comch_server_as_ctx(doca_comch_server_get_server_ctx(conn)),
					       &user_data);
		if (ret != DOCA_SUCCESS || user_data.ptr == nullptr) {
			DOCA_LOG_ERR("[BUG] unable to extract user data");
			return;
		}

		auto *self = static_cast<comch_server_control_channel *>(user_data.ptr);
		static_cast<void>(doca_comch_connection_set_user_data(conn, doca_data{.ptr = self}));
		self->m_connection = conn;
	}

	static void event_connection_disconnected_cb(doca_comch_event_connection_status_changed *event,
						     doca_comch_connection *conn,
						     uint8_t change_successful) noexcept
	{
		static_cast<void>(event);
		static_cast<void>(change_successful);

		doca_data user_data{.ptr = nullptr};
		auto const ret =
			doca_ctx_get_user_data(doca_comch_server_as_ctx(doca_comch_server_get_server_ctx(conn)),
					       &user_data);
		if (ret != DOCA_SUCCESS || user_data.ptr == nullptr) {
			DOCA_LOG_ERR("[BUG] unable to extract user data");
			return;
		}

		auto *self = static_cast<comch_server_control_channel *>(user_data.ptr);
		if (self->m_connection != conn) {
			DOCA_LOG_WARN("Ignoring disconnect of non-connected connection");
			return;
		}

		self->m_connection = nullptr;
	}
};

class tcp_client_control_channel : public tcp_control_channel {
public:
	~tcp_client_control_channel() override
	{
		try {
			m_socket.close();
		} catch (storage::runtime_error const &ex) {
			DOCA_LOG_ERR("Failed to close socket: %s", ex.what());
		}
	}

	tcp_client_control_channel() = delete;
	explicit tcp_client_control_channel(storage::ip_address const &server_address)
		: tcp_control_channel{},
		  m_server_address{server_address},
		  m_socket{}
	{
		m_socket.connect(m_server_address);
	}
	tcp_client_control_channel(tcp_client_control_channel const &) = delete;
	tcp_client_control_channel(tcp_client_control_channel &&) noexcept = delete;
	tcp_client_control_channel &operator=(tcp_client_control_channel const &) = delete;
	tcp_client_control_channel &operator=(tcp_client_control_channel &&) noexcept = delete;

	bool is_connected() override
	{
		std::string const remote_display_string =
			m_server_address.get_address() + ":" + std::to_string(m_server_address.get_port());

		switch (m_socket.poll_is_connected()) {
		case storage::tcp_socket::connection_status::connected: {
			DOCA_LOG_INFO("Connected to %s", remote_display_string.c_str());
			return true;
		}
		case storage::tcp_socket::connection_status::establishing: {
		} break;
		case storage::tcp_socket::connection_status::refused: {
			m_socket = storage::tcp_socket{}; /* reset the socket */
			m_socket.connect(m_server_address);
		} break;
		case storage::tcp_socket::connection_status::failed: {
			throw storage::runtime_error{DOCA_ERROR_CONNECTION_ABORTED,
						     "Unable to connect to " + remote_display_string};
		}
		}

		return false;
	}

	void send_message(message const &msg) override
	{
		DOCA_LOG_DBG("%s", to_string(msg).c_str());
		write_to_tcp_socket(m_socket, msg);
	}

	storage::control::message *poll() override
	{
		auto *msg = poll_tcp_socket(m_socket);
		if (msg) {
			DOCA_LOG_DBG("%s", to_string(*msg).c_str());
		}

		return msg;
	}

private:
	storage::ip_address m_server_address;
	storage::tcp_socket m_socket;
};

class tcp_server_control_channel : public tcp_control_channel {
public:
	~tcp_server_control_channel() override
	{
		try {
			m_client_socket.close();
		} catch (storage::runtime_error const &ex) {
			DOCA_LOG_ERR("Failed to close socket: %s", ex.what());
		}
		try {
			m_listen_socket.close();
		} catch (storage::runtime_error const &ex) {
			DOCA_LOG_ERR("Failed to close socket: %s", ex.what());
		}
	}
	tcp_server_control_channel() = delete;
	explicit tcp_server_control_channel(uint16_t listen_port)
		: tcp_control_channel{},
		  m_listen_socket{},
		  m_client_socket{}
	{
		m_listen_socket.listen(listen_port);
	}
	tcp_server_control_channel(tcp_server_control_channel const &) = delete;
	tcp_server_control_channel(tcp_server_control_channel &&) noexcept = delete;
	tcp_server_control_channel &operator=(tcp_server_control_channel const &) = delete;
	tcp_server_control_channel &operator=(tcp_server_control_channel &&) noexcept = delete;

	bool is_connected() override
	{
		m_client_socket = m_listen_socket.accept();
		return m_client_socket.is_valid();
	}

	void send_message(message const &msg) override
	{
		DOCA_LOG_DBG("%s", to_string(msg).c_str());
		write_to_tcp_socket(m_client_socket, msg);
	}

	storage::control::message *poll() override
	{
		auto *msg = poll_tcp_socket(m_client_socket);
		if (msg) {
			DOCA_LOG_DBG("%s", to_string(*msg).c_str());
		}

		return msg;
	}

private:
	storage::tcp_socket m_listen_socket;
	storage::tcp_socket m_client_socket;
};

} // namespace

std::unique_ptr<storage::control::comch_channel> make_comch_client_control_channel(
	doca_dev *dev,
	char const *channel_name,
	void *callback_user_data,
	comch_channel::consumer_event_callback new_consumer_event_cb,
	comch_channel::consumer_event_callback expired_consumer_event_cb)
{
	return std::make_unique<comch_client_control_channel>(dev,
							      channel_name,
							      callback_user_data,
							      std::move(new_consumer_event_cb),
							      std::move(expired_consumer_event_cb));
}

std::unique_ptr<storage::control::comch_channel> make_comch_server_control_channel(
	doca_dev *dev,
	doca_dev_rep *dev_rep,
	char const *channel_name,
	void *callback_user_data,
	comch_channel::consumer_event_callback new_consumer_event_cb,
	comch_channel::consumer_event_callback expired_consumer_event_cb)
{
	return std::make_unique<comch_server_control_channel>(dev,
							      dev_rep,
							      channel_name,
							      callback_user_data,
							      std::move(new_consumer_event_cb),
							      std::move(expired_consumer_event_cb));
}

std::unique_ptr<storage::control::channel> make_tcp_client_control_channel(storage::ip_address const &server_address)
{
	return std::make_unique<tcp_client_control_channel>(server_address);
}

std::unique_ptr<storage::control::channel> make_tcp_server_control_channel(uint16_t listen_port)
{
	return std::make_unique<tcp_server_control_channel>(listen_port);
}

} // namespace storage::control