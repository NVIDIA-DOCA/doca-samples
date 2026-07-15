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

/* GPM = Generic Persistent Map */
#ifndef NFS_FSDEV_GPM_HPP
#define NFS_FSDEV_GPM_HPP

#include <unordered_map>
#include <string>
#include <vector>
#include <cassert>
#include <iostream>

#define GPM_MAGIC_NUMBER 0x12345678 ///< Magic number for persistent DB validation
#define GPM_END_OF_LIST -1	    ///< End of list marker
#define GPM_MAX_SIZE_DB 200000	    ///< Maximum number of entries in DB
#define GPM_INVALID -1		    ///< Invalid index
#define GPM_NA -2		    ///< Not available marker
#define GPM_MAX_RIGHT_LENGTH 300    ///< Max length for right key data

/**
 * @brief Volatile in-memory map for fast lookups.
 */
class VolatileMap {
private:
	std::unordered_map<unsigned long, std::pair<std::string, int>> m_left_key_map;
	std::unordered_map<std::string, std::pair<unsigned long, int>> m_right_key_map;

	bool Remove(unsigned long left, const std::string &right)
	{
		if (FindIndexViaLeftKey(left) == (int)GPM_INVALID || FindIndexViaRightKey(right) == (int)GPM_INVALID) {
			return false;
		}

		auto it1 = m_left_key_map.find(left);
		m_left_key_map.erase(it1);

		auto it2 = m_right_key_map.find(right);
		m_right_key_map.erase(it2);
		return true;
	}

public:
	VolatileMap(void) : m_left_key_map(), m_right_key_map()
	{
	}
	VolatileMap(const VolatileMap &other) = delete;
	VolatileMap &operator=(const VolatileMap &other) = delete;
	~VolatileMap() = default;

	/**
	 * @brief Insert a new entry.
	 */
	bool Insert(unsigned long left, const std::string &right, unsigned long persistent_db_index)
	{
		if (FindIndexViaLeftKey(left) != (int)GPM_INVALID || FindIndexViaRightKey(right) != (int)GPM_INVALID) {
			return false;
		}

		m_left_key_map[left] = std::make_pair(right, persistent_db_index);
		m_right_key_map[right] = std::make_pair(left, persistent_db_index);
		return true;
	}

	bool RemoveByLeftKey(unsigned long left)
	{
		if (FindIndexViaLeftKey(left) == (int)GPM_INVALID) {
			return false;
		}

		std::string right = m_left_key_map[left].first;
		return Remove(left, right);
	}

	bool RemoveByRightKey(const std::string &right)
	{
		if (FindIndexViaRightKey(right) == (int)GPM_INVALID) {
			return false;
		}

		unsigned long left = m_right_key_map[right].first;
		return Remove(left, right);
	}

	int FindIndexViaLeftKey(unsigned long left) const
	{
		auto it = m_left_key_map.find(left);
		if (it != m_left_key_map.end()) {
			return it->second.second;
		}

		return (int)GPM_INVALID;
	}

	int FindIndexViaRightKey(std::string right) const
	{
		auto it = m_right_key_map.find(right);
		if (it != m_right_key_map.end()) {
			return it->second.second;
		}

		return (int)GPM_INVALID;
	}
};

/**
 * @brief Entry for persistent map, supports double-buffering for versioning.
 * @tparam T Data type stored in the entry.
 */
template <typename T>
class Entry {
private:
	int m_next;
	unsigned long m_left;
	char m_right_data[GPM_MAX_RIGHT_LENGTH];
	int m_right_len;
	T m_data[2];
	int m_version;

public:
	Entry(int next, unsigned long left, std::string right, const T &value)
		: m_next(next),
		  m_left(left),
		  m_version(0)
	{
		m_right_len = right.length();
		memcpy(m_right_data, right.data(), m_right_len);
		m_data[0] = value;
		m_data[1] = value;
	}

	~Entry() = default;

	/**
	 * @brief Update the entry with a new version.
	 */
	void Update(const T &new_version)
	{
		memcpy(&(m_data[(m_version + 1) % 2]), &new_version, sizeof(T));
		asm volatile("" ::: "memory");
		m_version = (m_version + 1) % 2;
	}

	T GetData(void)
	{
		return m_data[m_version];
	}
	void SetNext(int next)
	{
		m_next = next;
	}
	int GetNext(void)
	{
		return m_next;
	}
	unsigned long GetLeftKey()
	{
		return m_left;
	}
	std::string GetRightKey()
	{
		return std::string(m_right_data, m_right_len);
	}
};

/**
 * @brief Raw persistent database holding entries.
 * @tparam T Data type stored in the database.
 */
template <typename T>
class RawPersistentDataBase {
public:
	int m_magic;
	int m_free_list_head;
	unsigned long m_left_key_counter;
	Entry<T> m_entries[GPM_MAX_SIZE_DB];
};

/**
 * @brief Persistent map supporting insert, update, remove, and lookup.
 * @tparam T Data type stored in the map.
 */
template <typename T>
class PersistentMap {
private:
	RawPersistentDataBase<T> *m_raw_persistent_data_base;
	VolatileMap m_volatile_map;

	void Init()
	{
		m_raw_persistent_data_base->m_left_key_counter = 1;
		m_raw_persistent_data_base->m_free_list_head = 0;
		for (int i = 0; i < GPM_MAX_SIZE_DB; ++i) {
			m_raw_persistent_data_base->m_entries[i].SetNext(
				(i == (int)GPM_MAX_SIZE_DB - 1) ? (int)GPM_END_OF_LIST : i + 1);
		}

		asm volatile("" ::: "memory");
		m_raw_persistent_data_base->m_magic = GPM_MAGIC_NUMBER;
	}

	void Restore()
	{
		std::vector<char> is_free_arr(GPM_MAX_SIZE_DB, 0);

		int curr_index = m_raw_persistent_data_base->m_free_list_head;
		while (curr_index != GPM_END_OF_LIST) {
			is_free_arr[curr_index] = 1;
			curr_index = m_raw_persistent_data_base->m_entries[curr_index].GetNext();
		}

		for (size_t i = 0; i < GPM_MAX_SIZE_DB; ++i) {
			if (!is_free_arr[i]) {
				unsigned long left = m_raw_persistent_data_base->m_entries[i].GetLeftKey();
				std::string right = m_raw_persistent_data_base->m_entries[i].GetRightKey();

				bool res = m_volatile_map.Insert(left, right, i);
				if (!res) {
					std::cout << "Error in restoring Entry " << i << "left=[" << left << "] right=["
						  << right << "]" << std::endl;
					exit(-1);
				}
			}
		}
	}

public:
	PersistentMap(void *shmem_db) : m_volatile_map()
	{
		m_raw_persistent_data_base = static_cast<RawPersistentDataBase<T> *>(shmem_db);
		if (m_raw_persistent_data_base->m_magic != GPM_MAGIC_NUMBER) {
			Init();
			asm volatile("" ::: "memory");
		} else {
			Restore();
		}
	}

	PersistentMap(const PersistentMap &other) = delete;
	PersistentMap &operator=(const PersistentMap &other) = delete;
	~PersistentMap() = default;

	// will return false if and only if there is already entry like this
	bool InsertEntry(const T &entry, unsigned long left, const std::string &right)
	{
		int index1 = m_volatile_map.FindIndexViaLeftKey(left);
		int index2 = m_volatile_map.FindIndexViaRightKey(right);

		assert(index1 == index2);

		if (index1 != (int)GPM_INVALID || index2 != (int)GPM_INVALID) {
			std::cout << "Error: Entry Already in volatile map" << std::endl;
			std::cout << "index1 = " << index1 << " , index2 = " << index2 << " " << std::endl;
			return false;
		}

		int free_cell = m_raw_persistent_data_base->m_free_list_head;
		assert(free_cell != GPM_END_OF_LIST);
		int new_head = m_raw_persistent_data_base->m_entries[free_cell].GetNext();
		m_volatile_map.Insert(left, right, free_cell);
		new (&m_raw_persistent_data_base->m_entries[free_cell]) Entry<T>(GPM_NA, left, right, entry);

		asm volatile("" ::: "memory");
		m_raw_persistent_data_base->m_free_list_head = new_head;
		asm volatile("" ::: "memory");

		return true;
	}

	bool UpdateEntryByLeft(const T &entry, unsigned long left)
	{
		int index = m_volatile_map.FindIndexViaLeftKey(left);
		if (index == (int)GPM_INVALID) {
			return false;
		}

		m_raw_persistent_data_base->m_entries[index].Update(entry);
		asm volatile("" ::: "memory");
		return true;
	}

	bool UpdateEntryByRight(const T &entry, const std::string &right)
	{
		int index = m_volatile_map.FindIndexViaRightKey(right);
		if (index == (int)GPM_INVALID) {
			return false;
		}

		m_raw_persistent_data_base->m_entries[index].Update(entry);
		asm volatile("" ::: "memory");

		return true;
	}

	bool RemoveEntryByLeft(unsigned long left)
	{
		int index = m_volatile_map.FindIndexViaLeftKey(left);
		if (index == (int)GPM_INVALID) {
			return false;
		}

		if (m_volatile_map.RemoveByLeftKey(left) == false) {
			return false;
		}

		m_raw_persistent_data_base->m_entries[index].SetNext(m_raw_persistent_data_base->m_free_list_head);

		asm volatile("" ::: "memory");
		m_raw_persistent_data_base->m_free_list_head = index;
		asm volatile("" ::: "memory");

		return true;
	}

	bool RemoveEntryByRight(const std::string &right)
	{
		int index = m_volatile_map.FindIndexViaRightKey(right);
		if (index == (int)GPM_INVALID) {
			return false;
		}

		if (m_volatile_map.RemoveByRightKey(right) == false) {
			return false;
		}

		m_raw_persistent_data_base->m_entries[index].SetNext(m_raw_persistent_data_base->m_free_list_head);

		asm volatile("" ::: "memory");
		m_raw_persistent_data_base->m_free_list_head = index;
		asm volatile("" ::: "memory");

		return true;
	}

	T GetEntryByLeft(unsigned long left) const
	{
		int index = m_volatile_map.FindIndexViaLeftKey(left);
		assert(index != (int)GPM_INVALID);
		return m_raw_persistent_data_base->m_entries[index].GetData();
	}

	T GetEntryByRight(const std::string &right) const
	{
		int index = m_volatile_map.FindIndexViaRightKey(right);
		assert(index != (int)GPM_INVALID);
		return m_raw_persistent_data_base->m_entries[index].GetData();
	}

	unsigned long GenerateLeftKey(void)
	{
		return ++m_raw_persistent_data_base->m_left_key_counter;
	}

	bool CheckEntryExistByLeftKey(unsigned long left) const
	{
		int index = m_volatile_map.FindIndexViaLeftKey(left);
		return (index != (int)GPM_INVALID);
	}

	bool CheckEntryExistByRightKey(const std::string &right) const
	{
		int index = m_volatile_map.FindIndexViaRightKey(right);
		return (index != (int)GPM_INVALID);
	}
};

#endif // NFS_FSDEV_GPM_HPP