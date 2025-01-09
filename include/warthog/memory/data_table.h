#ifndef WARTHOG_MEMORY_DATA_TABLE_H
#define WARTHOG_MEMORY_DATA_TABLE_H

#include <cstring>
#include <cstdint>
#include <cassert>
#include <cstdlib>
#include <optional>
#include <limits>
#include <variant>
#include <string_view>
#include <memory_resource>
#include <bit>
#include <stdexcept>
#include <algorithm>
#include <typeindex>

namespace warthog::memory
{

// careful of type::STRING, must be managed by owner of data_column
class data_column
{
public:
	enum class type : uint8_t
	{
		STRING,
		INT,
		FLOAT,
	};
	union data_type
	{
		static constexpr int sso_width = 3;
		static constexpr int sso_mask = 0b111;
		union {
			const char* ptr;
			char sso[8];
		} s;
		int64_t i;
		double f;
	};
	using variant_type = std::variant<std::nullptr_t, int64_t, double, std::string_view>;
	using size_type = uint32_t;
	using value_type = data_type;

	constexpr data_column() = default;
	data_column(std::pmr::memory_resource* res) : m_res(res)
	{ }
	~data_column()
	{
		if (m_data != nullptr) {
			dealloc_(m_data, m_reserved);
			m_data = nullptr;
		}
	}

	constexpr value_type& operator[](size_type pos) noexcept
	{
		assert(pos < m_size);
		return m_data[pos];
	}
	constexpr value_type operator[](size_type pos) const noexcept
	{
		assert(pos < m_size);
		return m_data[pos];
	}

	constexpr value_type& at(size_type pos)
	{
		if (pos >= m_size)
			throw std::out_of_range("pos");
		return m_data[pos];
	}
	constexpr value_type at(size_type pos) const
	{
		if (pos >= m_size)
			throw std::out_of_range("pos");
		return m_data[pos];
	}

	template <type DataType>
	constexpr variant_type as(size_type pos) const
	{
		return to_varient(at(pos), DataType);
	}
	constexpr variant_type as(size_type pos, type data_type) const
	{
		return to_varient(at(pos), data_type);
	}

	constexpr value_type& front() noexcept
	{
		assert(m_size > 0);
		return m_data[0];
	}
	constexpr value_type front() const noexcept
	{
		assert(m_size > 0);
		return m_data[0];
	}
	constexpr value_type& back() noexcept
	{
		assert(m_size > 0);
		return m_data[m_size-1];
	}
	constexpr value_type back() const noexcept
	{
		assert(m_size > 0);
		return m_data[m_size-1];
	}
	constexpr value_type* data() noexcept
	{
		return m_data;
	}
	constexpr const value_type* data() const noexcept
	{
		return m_data;
	}

	constexpr void resize(size_type count)
	{
		resize(count, null_value());
	}
	constexpr void resize(size_type count, value_type value)
	{
		if (count > m_size) {
			auto_reserve_(count);
			std::fill(m_data + m_size, m_data + count, value);
		}
		m_size = count;
	}
	constexpr void reserve(size_type count)
	{
		reserve_(count);
	}

	static constexpr bool is_int_null(value_type value) noexcept
	{
		return value.i == std::numeric_limits<int64_t>::min();
	}
	static constexpr bool is_float_null(value_type value) noexcept
	{
		return value.f == std::numeric_limits<double>::lowest();
	}
	static constexpr bool is_string_null(value_type value) noexcept
	{
		return value.s.ptr == nullptr;
	}
	static constexpr std::string_view get_string_ptr(data_type value) noexcept
	{
		// handles sso
		if constexpr (std::endian::native == std::endian::little) {
			// little endian, value.s.sso[0] holds bits 0-7
			if ((value.s.sso[0] & data_type::sso_mask)) {
				assert((value.s.sso[0] & data_type::sso_mask) == value.s.sso[0]);
				return std::string_view(&value.s.sso[1], value.s.sso[0]);
			}
		} else {
			// big endian, value.s.sso[7] holds bits 0-7
			if ((value.s.sso[7] & data_type::sso_mask)) {
				assert((value.s.sso[7] & data_type::sso_mask) == value.s.sso[7]);
				return std::string_view(&value.s.sso[0], value.s.sso[7]);
			}
		}
		// no sso
		if (value.s.ptr == nullptr)
			return std::string_view();
		size_t count{};
		std::memcpy(&count, value.s.ptr - sizeof(size_t), sizeof(size_t));
		return std::string_view(value.s.ptr, count);
	}

	static constexpr variant_type to_varient(data_type value, type t) noexcept
	{
		assert(t == type::STRING || t == type::INT || t == type::FLOAT);
		switch (t) {
		case type::STRING:
			return is_string_null(value) ? variant_type() : get_string_ptr(value);
		case type::INT:
			return is_int_null(value) ? variant_type() : variant_type(value.i);
		case type::FLOAT:
			return is_float_null(value) ? variant_type() : variant_type(value.f);
		default:
			assert(false);
			return variant_type();
		}
	}

	static constexpr value_type null_value() noexcept
	{
		return value_type{ .s.ptr = nullptr };
	}
	static constexpr data_type from_int(std::optional<int64_t> value = std::nullopt) noexcept
	{
		return data_type{ .i = value.value_or(std::numeric_limits<int64_t>::min()) };
	}
	static constexpr data_type from_float(std::optional<double> value = std::nullopt) noexcept
	{
		return data_type{ .f = value.value_or(std::numeric_limits<double>::lowest()) };
	}
	/**
	 * Return value in data_type if fits in sso (6 characters)
	 * Otherwise, return data_type as null.
	 * Returns null if value is sized 0.
	 */
	static constexpr data_type try_from_string(std::string_view value = std::string_view()) noexcept
	{
		if (value.size() == 0 || value.size() > 6)
			return null_value();
		data_type sso{ .s.sso = {} };
		if constexpr (std::endian::native == std::endian::little) {
			// little endian, value.s.sso[0] holds bits 0-7
			sso.s.sso[0] = static_cast<char>(static_cast<int>(value.size()));
			value.copy(&sso.s.sso[1], value.size());
			assert(sso.s.sso[1 + value.size()] == '\0'); // nullptr already set from zero init
		} else {
			// big endian, value.s.sso[7] holds bits 0-7
			sso.s.sso[7] = static_cast<char>(static_cast<int>(value.size()));
			value.copy(&sso.s.sso[0], value.size());
			assert(sso.s.sso[value.size()] == '\0'); // nullptr already set from zero init
		}
		return sso;
	}
	/**
	 * Care must be taken.
	 * value must be aligned to at least 8 bytes
	 * length must be in size_t word before value
	 * value can also be nullptr.
	 */
	static constexpr data_type try_from_char_(char* value) noexcept
	{
		assert((std::bit_cast<uintptr_t>(value) & data_type::sso_mask) == 0);
		return data_type{.s.ptr = value};
	}

protected:
	constexpr data_type* alloc_(size_type count)
	{
		assert(count > 0);
		if (m_res) {
			return static_cast<data_type*>( m_res->allocate(count * sizeof(data_type), alignof(data_type)) );
		} else {
			return static_cast<data_type*>( std::malloc(count * sizeof(data_type)) );
		}
	}
	constexpr void dealloc_(data_type* ptr, size_type count)
	{
		assert(count > 0);
		if (m_res) {
			m_res->deallocate(ptr, count * sizeof(data_type), alignof(data_type));
		} else {
			std::free(ptr);
		}
	}
	constexpr void auto_reserve_(size_type count)
	{
		if (count > m_reserved) {
			int shift = std::bit_width(count);
			shift = std::min(shift - 2, 3); // minimum shift size of 8
			reserve_(m_reserved + (static_cast<size_type>(1) << shift));
		}
	}
	constexpr void reserve_(size_type count)
	{
		if (count > m_reserved) {
			data_type* old_data = m_data;
			data_type* new_data = alloc_(count);
			std::copy_n(old_data, m_size, new_data);
			dealloc_(old_data, m_reserved);
			m_data = new_data;
			m_reserved = count;
		}
	}

protected:
	std::pmr::memory_resource* m_res = nullptr;
	data_type* m_data = nullptr;
	uint32_t m_size = 0;
	uint32_t m_reserved = 0;
};

class data_table
{
public:
	struct column
	{
		std::type_index category;
		const char* name;
		uint32_t name_size;
		data_column::type type;
		data_column data;
	};
};

} // namespace warthog::memory

#endif // WARTHOG_MEMORY_DATA_TABLE_H
