/*
 * RingBuffer.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef EMBDEVENV_RING_BUFFER_HXX
#define EMBDEVENV_RING_BUFFER_HXX

#include <vector>
#include <deque>
#include <mutex>
#include <cstddef>
#include <algorithm>

namespace embdevenv {

template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity = 1000)
        : _capacity(capacity > 0 ? capacity : 1)
    {
    }

    ~RingBuffer() = default;

    void push(const T &item)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_buffer.size() >= _capacity) {
            _buffer.pop_front();
        }
        _buffer.push_back(item);
    }

    void push(T &&item)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_buffer.size() >= _capacity) {
            _buffer.pop_front();
        }
        _buffer.push_back(std::move(item));
    }

    std::vector<T> getLatest(size_t count) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::vector<T> result;
        size_t available = _buffer.size();
        size_t take = std::min(count, available);
        result.reserve(take);

        auto it = _buffer.end() - take;
        for (; it != _buffer.end(); ++it) {
            result.push_back(*it);
        }
        return result;
    }

    std::vector<T> getAll() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return std::vector<T>(_buffer.begin(), _buffer.end());
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _buffer.clear();
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _buffer.size();
    }

    size_t capacity() const
    {
        return _capacity;
    }

    void setCapacity(size_t newCapacity)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _capacity = newCapacity > 0 ? newCapacity : 1;
        while (_buffer.size() > _capacity) {
            _buffer.pop_front();
        }
    }

private:
    size_t _capacity;
    std::deque<T> _buffer;
    mutable std::mutex _mutex;
};

} // namespace embdevenv

#endif // EMBDEVENV_RING_BUFFER_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
