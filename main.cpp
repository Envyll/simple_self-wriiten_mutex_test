#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdint>
#include <atomic>
#include <expected>

#include <print>
#include <thread>

// it is one byte size.. isn't it cool?
struct my_mutex {
public:
	using internal_type = uint8_t;
	using ref_type = std::atomic_ref<internal_type>;
	static_assert(ref_type::is_always_lock_free);

	struct lock_t {
		friend class my_mutex;

		explicit operator bool () const {
			return (bool)self;
		}

		explicit lock_t() = default;

		lock_t(lock_t&& that) noexcept {
			this->self.swap(that.self);
		};

		lock_t& operator=(lock_t&& that) noexcept {
			this->~lock_t();
			new(this) auto(std::move(that));

			return *this;
		}

		~lock_t() {
			if (self && !self->unlock_()) {
				std::abort();
			}
		}

		private:
		std::optional<my_mutex const&> self;

		explicit lock_t(my_mutex const& m) noexcept : self(m) {}
	};

private:
	using Self = my_mutex;
	using SelfRef = Self const&;

	mutable internal_type _lock = 0;

	[[nodiscard]]
	auto as_atomic(this const Self& self) {
		return ref_type(self._lock);
	}

	[[nodiscard]]
	auto address_u32(this const Self& self) -> std::uintptr_t {
		return reinterpret_cast<uintptr_t>(std::addressof(self._lock)) & ~0b11;
	}

	[[nodiscard]]
	uint32_t get_bitset(this const Self& self) {
		// assuming little endian
		return 0xffu << (reinterpret_cast<uintptr_t>(std::addressof(self._lock)) & 0b11) * 8;
	}

	[[nodiscard]]
	uint32_t get_expected(this const Self& self) {
		// assuming little endian
		return 0x02u << (reinterpret_cast<uintptr_t>(std::addressof(self._lock)) & 0b11) * 8;
	}

	bool unlock_(this const Self& self) {
		auto result = self.as_atomic().exchange(0);
		if (result == 2) {
			syscall(SYS_futex, self.address_u32(), FUTEX_WAKE_BITSET_PRIVATE, 1, nullptr, nullptr, self.get_bitset());
		}
		return result;
	}

	[[nodiscard]]
	bool try_lock_(this const Self& self) {
		internal_type expected = 0;

		return self.as_atomic().compare_exchange_weak(expected, 1);
	}

	void lock_(this const Self& self) {
		while (!self.try_lock_()) {
			self.as_atomic().store(2);
			syscall(SYS_futex, self.address_u32(), FUTEX_WAIT_BITSET_PRIVATE, self.get_expected(), nullptr, nullptr, self.get_bitset());
		}
	}

public:
	my_mutex(SelfRef) = delete;
	auto operator=(SelfRef) = delete;
	~my_mutex() {
		if (as_atomic().load()) {
			abort();
		}
	}

	my_mutex() noexcept = default;

	lock_t try_lock(this SelfRef self) {
		return self.try_lock_() ? lock_t(self) : lock_t();
	}

	lock_t lock(this SelfRef self) {
		self.lock_();
		return lock_t(self);
	}
};

int main()  {
	my_mutex m;
	int x = 0;
	using namespace std::chrono_literals;

	auto _ = std::jthread([&] {
		auto _ = std::jthread([&] {
			std::this_thread::sleep_for(250ms);

			if (auto _ = m.try_lock()) {
				std::println("{} : x = {}", __LINE__, x);
				x = -1;
				std::println("{} : x = {}", __LINE__, x);
			} else {
				std::println("{} : locked", __LINE__, x);
			}
		});

		auto _ = std::jthread([&] {
			std::this_thread::sleep_for(400ms);

			if (auto _ = m.try_lock()) {
				std::println("{} : x = {}", __LINE__, x);
				x = 3;
				std::println("{} : x = {}", __LINE__, x);
			} else {
				std::println("{} : locked", __LINE__, x);
			}
		});

		auto _ = std::jthread([&] {
			std::this_thread::sleep_for(100ms);
			if (auto _ = m.lock()) {
				std::println("{} : x = {}", __LINE__, x);
				x = 2;
				std::println("{} : x = {}", __LINE__, x);
				std::this_thread::sleep_for(100ms);
			} else {
				abort();
			}
		});

		auto _ = m.lock();
		std::println("{} : x = {}", __LINE__, x);
		x = 1;
		std::println("{} : x = {}", __LINE__, x);
		std::this_thread::sleep_for(200ms);

	});

}