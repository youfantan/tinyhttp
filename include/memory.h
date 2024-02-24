#pragma once

#include <stacktrace.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <iostream>
#include <string>
#include <mutex>
#include <vector>
#include <new>
#include <shared_mutex>

class memory_exception : std::exception {
    std::string msg;
public:
    memory_exception(std::string&& who, std::string&& reason) : msg(std::format("met an memory exception when {} was called: {}", who, reason)) {}
    [[nodiscard]] const char *what() const noexcept override {
        return msg.c_str();
    }
    void print() const {
#ifdef ENABLE_ANSI_DISPLAY
        std::cout << "\033[31m" << std::endl;
#endif
        std::cout << msg << std::endl;
        std::cout << "[STACKTRACE]" << std::endl;
        std::cout << get_stack_trace() << std::endl;
#ifdef ENABLE_ANSI_DISPLAY
        std::cout << "\033[39m" << std::endl;
#endif
    }
};

template<typename T>
concept is_allocator = requires(T t)
{
    /* 该约束要求对象具有allocate(size_t size)的成员函数，该函数分配size大小的内存并返回一个void类型的指针指向该内存块起始。 */
    { t.allocate(SIZE_MAX) } -> std::same_as<void*>;
    /* 该约束要求对象具有reallocate(void* ptr, size_t size)的成员函数，将ptr指向的内存回收并重新分配size大小的内存，返回一个void类型的指针指向内存块起始。 */
    { t.reallocate(reinterpret_cast<void*>(new char()), SIZE_MAX) } -> std::same_as<void*>;
    /* 该约束要求上述两个成员函数在分配内存时记录下分配的指针和大小并在该函数统一释放。 */
    t.release();
    /* 该约束要求对象实现析构函数，并且在析构函数内应检查是否释放掉所有指针，如果没有那么在这里应重新释放，避免内存泄露。 */
    t.~T();
};

class heap_allocator {
private:
    std::vector<void*> ptrs_ {};
    std::mutex mtx_ {};
public:
    heap_allocator() = default;
    heap_allocator(const heap_allocator&) = delete;
    heap_allocator(heap_allocator&& allocator)  noexcept : ptrs_(std::move(allocator.ptrs_)), mtx_(std::mutex()) {
        allocator.ptrs_ = std::vector<void*>();
    }
    void* allocate(size_t size) {
        std::lock_guard lock(mtx_);
        void *ptr = malloc(size);
        if (ptr == nullptr) {
            throw memory_exception("heap_allocator::allocate()", "malloc() returned nullptr");
        }
        ptrs_.push_back(ptr);
        return ptr;
    }
    void* reallocate(void *ptr, size_t size) {
        std::lock_guard lock(mtx_);
        for (auto & recorded : ptrs_) {
            if (recorded == ptr) {
                recorded = realloc(ptr, size);
                if (recorded == nullptr) {
                    throw memory_exception("heap_allocator::reallocate()", "realloc() returned nullptr");
                }
                return recorded;
            }
        }
        return nullptr; // 未定义的行为，ptr一定能在ptrs_被找到
    }
    void release() {
        std::lock_guard lock(mtx_);
        for (auto& ptr: ptrs_) {
            free(ptr);
        }
        ptrs_.clear();
    }
    ~heap_allocator() {
        if (!ptrs_.empty()) {
            release(); // 避免double-free错误
        }
    }
};

class aligned_heap_allocator {
private:
    struct aligned_chunk {
        void* ptr;
        size_t offset;
    };

    std::vector<aligned_chunk> ptrs_ {};
    std::mutex mtx_;
    int align_ {};
public:
    explicit aligned_heap_allocator(int align) : align_(align) {}
    aligned_heap_allocator(const aligned_heap_allocator&) = delete;
    aligned_heap_allocator(aligned_heap_allocator&& allocator)  noexcept : ptrs_(std::move(allocator.ptrs_)), mtx_(std::mutex()), align_(allocator.align_) {
        allocator.ptrs_ = std::vector<aligned_chunk>();
    }
    void* allocate(size_t size) {
        std::lock_guard lock(mtx_);
        void *ptr = malloc(size + align_);
        if (ptr == nullptr) {
            throw memory_exception("aligned_heap_allocator::allocate()", "malloc() returned nullptr");
        }
        size_t offset = reinterpret_cast<size_t>(ptr) % align_;
        ptr = reinterpret_cast<void*>(reinterpret_cast<size_t>(ptr) + offset);
        ptrs_.push_back({ ptr, offset });
        return ptr;
    }
    void* reallocate(void* ptr, size_t size) {
        std::lock_guard lock(mtx_);
        for (auto & record : ptrs_) {
            if (record.ptr == ptr) {
                void* base = realloc(reinterpret_cast<void*>(reinterpret_cast<size_t>(record.ptr) - record.offset), size + align_);
                if (base == nullptr) {
                    throw memory_exception("aligned_heap_allocator::reallocate()", "realloc() returned nullptr");
                }
                size_t offset = reinterpret_cast<size_t>(base) % align_;
                record = {reinterpret_cast<void*>(reinterpret_cast<size_t>(base) + offset), offset};
                return record.ptr;
            }
        }
        return nullptr; // 未定义的行为，ptr一定能在ptrs_被找到
    }
    void release() {
        std::lock_guard lock(mtx_);
        for (auto& record: ptrs_) {
            free(record.ptr);
        }
        ptrs_.clear();
    }
    ~aligned_heap_allocator() {
        if (!ptrs_.empty()) {
            release(); // 避免double-free错误
        }
    }
};

template<typename T>
concept is_buffer = requires (T t) {
    { t.pointer() } -> std::same_as<char*>;
    { t.capacity() } -> std::same_as<size_t>;
    { t.rwlock() } -> std::same_as<std::shared_mutex&>;
};

template<typename T>
concept is_plain_type = requires {
    T(0);
};

template<typename Buffer>
class buffer_stream {
    static_assert(is_buffer<Buffer>);
private:
    Buffer& buffer_;
    size_t position_ {0};
    std::shared_mutex& rwlock_;
    bool eof_ {false};
    bool auto_expand {false};
public:
    explicit buffer_stream(Buffer& buffer) : buffer_(buffer), rwlock_(buffer_.rwlock()) {}

    void lock_read() const {
        rwlock_.lock_shared();
    }

    void lock_write() const {
        rwlock_.lock();
    }

    void unlock_read() const {
        rwlock_.unlock_shared();
    }

    void unlock_write() const {
        rwlock_.unlock();
    }

    /* 引用缓冲的某段数据，该函数仅基于size对所引用的数据进行越界检查并返回数据指针。 */
    char* reference(size_t offset, size_t size) {
        lock_read();
        if (offset + size > buffer_.capacity()) {
            unlock_read();
            return nullptr;
        }
        unlock_read();
        return buffer_.pointer() + offset;
    }

    /* 读取缓冲的某段数据，调用者应该自己分配dest指针，函数仅使用memcpy将数据复制到dest指针内。 */
    bool read(char* dest, const size_t offset, const size_t size) {
        lock_read();
        auto ref = reference(offset, size);
        if (ref != nullptr) {
            memcpy(dest, ref, size);
            unlock_read();
            return true;
        }
        unlock_read();
        return false;
    }

    /* 写入缓冲的某段数据。 */
    bool write(const char* src, const size_t offset, const size_t size) {
        lock_write();
        if (offset + size > buffer_.capacity()) {
            unlock_write();
            return false;
        }

        auto ptr = buffer_.pointer() + offset;
        memcpy(ptr, src, size);
        unlock_write();
        return true;
    }

    /* 按顺序(position_)以某个普通对象的大小读取该数据并复制到一个对象中返回。 */
    template<typename T>
    requires is_plain_type<T>
    T get() {
        constexpr auto step = sizeof(T);
        auto curr = position_ + step;
        if (curr == buffer_.capacity()) {
            eof_ = true;
        }
        position_ = curr;
        T object {0};
        if (!read(reinterpret_cast<char*>(&object), position_ - step, step)) {
            eof_ = true;
            return {0};
        }
        return object;
    }

    ssize_t get(char* dest, size_t size) {
        auto curr = position_ + size;
        if (eof_) {
            return -1;
        }
        if (curr >= buffer_.capacity()) {
            eof_ = true;
            auto remain = buffer_.capacity() - position_;
            read(dest, position_, remain);
            position_ = buffer_.capacity();
            return static_cast<ssize_t>(remain);
        }
        read(dest, position_, size);
        position_ = curr;
        return static_cast<ssize_t>(size);
    }

    std::string get() {
        auto ptr = buffer_.pointer() + position_;
        auto length = strlen(ptr);
        auto curr = position_ + length;
        if (curr == buffer_.capacity()) {
            eof_ = true;
        }
        position_ = curr;
        std::string s;
        s.resize_and_overwrite(length, [](char*, std::size_t size) {
            return size;
        });
        if (!read(s.data(), position_ - length, length)) {
            eof_ = true;
            return {};
        }
        return s;
    }

    template<typename T>
    requires is_plain_type<T>
    T& get_as() {
        constexpr auto step = sizeof(T);
        auto curr = position_ + step;
        if (curr == buffer_.capacity()) {
            eof_ = true;
        }
        position_ = curr;
        char* ptr;
        if ((ptr = reference(position_ - step, step)) == nullptr) {
            eof_ = true;
            throw memory_exception("buffer_stream::get_as<T>()", "segment fault, invalid pointer access");
        }
        T* obj_ptr = reinterpret_cast<T*>(ptr);
        return static_cast<T&>(*obj_ptr);
    }

    std::string_view get_as() {
        size_t length = 0;
        char* len_ptr;
        if ((len_ptr = reference(position_, sizeof(length))) == nullptr) {
            eof_ = true;
            return {};
        }
        char* data;
        length = *reinterpret_cast<size_t*>(len_ptr);
        if ((data = reference(position_ + sizeof(length), length)) == nullptr) {
            eof_ = true;
            return {};
        }
        position_ += sizeof(length) + length;
        return std::string_view(data, length);
    }

    template<typename T>
    bool append(const T& t) {
        constexpr auto step = sizeof(T);
        auto curr = position_ + step;
        if (auto_expand) {
            if (curr >= buffer_.capacity()) {
                buffer_.expand(curr);
            }
        }
        if (curr == buffer_.capacity()) {
            eof_ = true;
        }
        position_ = curr;
        if (!write(reinterpret_cast<const char*>(&t), position_ - step, step)) {
            eof_ = true;
            return false;
        }
        return true;
    }

    bool append(const std::string_view& s) {
        size_t original_length = s.length();
        const size_t length = original_length + sizeof(original_length);
        auto curr = position_ + length;
        if (auto_expand) {
            if (curr >= buffer_.capacity()) {
                buffer_.expand(curr);
            }
        }
        if (curr == buffer_.capacity()) {
            eof_ = true;
        }
        position_ = curr;
        if (!write(reinterpret_cast<char*>(&original_length), position_ - length, sizeof(original_length))) {
            eof_ = true;
            return false;
        }
        if (!write(s.data(), position_ - original_length, original_length)) {
            eof_ = true;
            return false;
        }
        return true;
    }
    bool append(const char* src, size_t size) {
        auto curr = position_ + size;
        if (auto_expand) {
            if (curr >= buffer_.capacity()) {
                buffer_.expand(curr);
            }
        }
        if (curr == buffer_.capacity()) {
            eof_ = true;
        }
        position_ = curr;
        if (!write(src, position_ - size, size)) {
            eof_ = true;
            return false;
        }
        return true;
    }

    void rewind() {
        lock_write();
        position_ = 0;
        unlock_write();
    }

    void set_auto_expand(bool enable) {
        auto_expand = enable;
    }

    bool eof() {
        return eof_;
    }

    void clear_eof() {
        eof_ = false;
    }

    void back(size_t len) {
        position_ -= len;
        if (position_ < buffer_.capacity()) {
            eof_ = false;
        }
    }
    void forward(size_t len) {
        position_ += len;
        if (position_ >= buffer_.capacity()) {
            eof_ = true;
        }
    }
};


/* 共享数组缓冲，该缓冲具有一个引用计数器，每次复制对象时相当于引用该缓冲区域的内存指针，并将引用计数值加一。对象被自动析构后会 */
template<typename Allocator>
requires is_allocator<Allocator>
class shared_array_buffer {
private:
    struct meta {
        char* ptr {};
        Allocator* allocator;
        size_t capacity {};
        std::mutex mutex;
        std::shared_mutex rwlock;
        int reference_count {};
    };
    meta* meta_;
public:
    /* 调用此构造函数要求allocator必须是分配在堆上的对象指针 */
    explicit shared_array_buffer(size_t capacity, Allocator* allocator) {
        try {
            meta_ = reinterpret_cast<meta*>(allocator->allocate(sizeof(meta)));
        } catch (memory_exception&) {
            throw;
        }
        memset(meta_, 0, sizeof(meta));
        meta_->allocator = allocator;
        meta_->capacity = capacity;
        try {
            meta_->ptr = reinterpret_cast<char*>(meta_->allocator->allocate(meta_->capacity));
        } catch (memory_exception&) {
            throw;
        }
        auto lock_ptr = new (&meta_->mutex)std::mutex;
        auto rwlock_ptr = new (&meta_->rwlock)std::shared_mutex;
        ++meta_->reference_count;
    }
    shared_array_buffer(const shared_array_buffer& buffer) : meta_(buffer.meta_) {
        meta_->mutex.lock(); // 未定义行为的影响体现在次行，如果出现问题把它移到if语句后
        if (meta_->reference_count == 0) {
            meta_->mutex.unlock();
            throw memory_exception("shared_array_buffer::shared_array_buffer(const shared_array_buffer&)", "source array buffer has released");
        }
        ++meta_->reference_count;
        meta_->mutex.unlock();
    }
    shared_array_buffer(shared_array_buffer&& buffer) noexcept : meta_(buffer.meta_) {
        buffer.meta_ = nullptr;
    }
    void expand(size_t new_capacity) {
        meta_->rwlock.lock();
        meta_->mutex.lock();
        meta_->capacity = new_capacity;
        meta_->ptr = reinterpret_cast<char*>(meta_->allocator->reallocate(meta_->ptr, meta_->capacity));
        meta_->mutex.unlock();
        meta_->rwlock.unlock();
    }
    ~shared_array_buffer() {
        /* 复制构造过程中若抛出异常那么在这里不进行析构，避免double-free错误 */
        if (&meta_->ptr != nullptr) {
            meta_->mutex.lock();
            --meta_->reference_count;
            if (meta_->reference_count == 0) {
                meta_->mutex.unlock();
                /*
                 * 请注意，在这里解锁操作被注释掉了，这代表该程序在使用一个相对安全的未定义行为来保障程序正常运行。一旦出现问题，应该取消注释掉该行。
                 */
                auto allocator_ptr = meta_->allocator;
                meta_->allocator->release();
                delete allocator_ptr;
            }
            meta_->mutex.unlock();
        }
    }

    char* pointer() {
        return meta_->ptr;
    }
    size_t capacity() {
        return meta_->capacity;
    }
    std::shared_mutex& rwlock() {
        return meta_->rwlock;
    }
};

/* 单例模式的数组缓冲，该缓冲仅可以通过移动构造的方式转移，不允许复制。 */
template<typename Allocator>
requires is_allocator<Allocator>
class unique_array_buffer {
private:
    Allocator allocator_;
    char* ptr_;
    size_t capacity_;
    std::shared_mutex rwlock_;
public:
    explicit unique_array_buffer(size_t capacity, Allocator allocator = Allocator()) : capacity_(capacity), allocator_(std::move(allocator)) {
        try {
            ptr_ = reinterpret_cast<char*>(allocator_.allocate(capacity_));
        } catch (memory_exception& e) {
            throw;
        }
        memset(ptr_, 0, capacity_);
    }
    unique_array_buffer(unique_array_buffer&& buffer)  noexcept : allocator_(std::move(buffer.allocator_)), ptr_(buffer.ptr_), capacity_(buffer.capacity_) {
        buffer.ptr_ = nullptr;
        buffer.capacity_ = 0;
    }
    /* 为数组缓冲扩容 */
    void expand(size_t new_capacity) {
        try {
            ptr_ = reinterpret_cast<char*>(allocator_.reallocate(ptr_, new_capacity));
        } catch (memory_exception& e) {
            throw;
        }
        capacity_ = new_capacity;
    }
    /* 手动释放 */
    void release() {
        if (ptr_ != nullptr) {
            allocator_.release();
        }
    }
    /* 自动释放 */
    ~unique_array_buffer() {
        if (ptr_ != nullptr) {
            allocator_.release();
        }
    }

    char* pointer() const {
        return ptr_;
    }
    size_t capacity() const {
        return capacity_;
    }
    std::shared_mutex& rwlock() {
        return rwlock_;
    }

    unique_array_buffer() = delete;
    unique_array_buffer(const unique_array_buffer&) = delete;
    unique_array_buffer operator=(const unique_array_buffer&) = delete;
    unique_array_buffer operator=(unique_array_buffer&&) = delete;
};

using general_array_buffer_t = unique_array_buffer<heap_allocator>;
using general_shared_array_buffer_t = shared_array_buffer<heap_allocator>;