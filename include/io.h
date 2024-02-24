#pragma once

#include <stacktrace.h>

#include <string>
#include <exception>
#include <format>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <unistd.h>
#include <sys/socket.h>


class io_exception : public std::exception {
    std::string msg;
public:
    io_exception(std::string&& who, std::string&& reason) : msg(std::format("met an IO exception when {} was called: {}", who, reason)) {}
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

inline std::string get_file_type(std::filesystem::file_type& type) {
    switch (type) {
        case std::filesystem::file_type::none: return "None";
        case std::filesystem::file_type::not_found: return "Not found";
        case std::filesystem::file_type::regular: return "Regular File";
        case std::filesystem::file_type::directory: return "Directory";
        case std::filesystem::file_type::symlink: return "Symbol link";
        case std::filesystem::file_type::block: return "Block device";
        case std::filesystem::file_type::character: return "Byte device";
        case std::filesystem::file_type::fifo: return "FIFO file";
        case std::filesystem::file_type::socket: return "Socket file";
        case std::filesystem::file_type::unknown: return "Unknown";
        default: return "Unknown";
    }
}

inline void check_path_exists(std::filesystem::path path, std::filesystem::file_type type) {
    if (!exists(path)) {
        throw io_exception("check_path_exists()", std::format("file {} not exists", path.c_str()));
    }
    std::filesystem::directory_entry entry(path);
    if (entry.status().type() != type) {
        throw io_exception("check_path_exists()", std::format("file {} is not {}", path.c_str(), get_file_type(type)));
    }
}

inline void writefd(int fd, char* buf, size_t size) {
    size_t left = size;
    char* off = buf;
    while (left > 0) {
        auto bytes_write = write(fd, off, left);
        if (bytes_write == -1) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            throw io_exception("writefd()", std::format("Write fd {} met an error {}. Bytes left: {}, Total: {}", fd, strerror(errno), left, size));
        }
        left -= bytes_write;
        off += bytes_write;
    }
}

inline void readfd(int fd, char* buf, size_t size) {
    size_t left = size;
    char* off = buf;
    while (left > 0) {
        auto bytes_read = read(fd, off, left);
        if (bytes_read == -1) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            throw io_exception("readfd()", std::format("Read fd {} met an error {}. Bytes left: {}, Total: {}", fd, strerror(errno), left, size));
        }
        left -= bytes_read;
        off += bytes_read;
    }
}

class nonblocking_socket_stream {
private:
    int fd_;
public:
    explicit nonblocking_socket_stream(int fd) : fd_(fd) {}
    [[nodiscard]] general_array_buffer_t read() const {
        general_array_buffer_t buffer(1024);
        buffer_stream stream(buffer);
        stream.set_auto_expand(true);
        char tmp[1024];
        while (true) {
            auto r = recv(fd_, tmp, 1024, 0);
            if (r == -1) {
                if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                    break;
                }
            } else if (r == 0) {
                //遇到SOCKET EOF标记，抛出一个EOF异常
                throw io_exception("nonblocking_socket_stream::read()", "met EOF");
            } else {
                if (!stream.append(tmp, r)) {
                    throw io_exception("nonblocking_socket_stream::read()", "cannot write buffer");
                }
            }
        }
        return buffer;
    }
    void write(general_array_buffer_t buffer) {
        buffer_stream stream(buffer);
        std::string_view str = stream.get_as();
        writefd(fd_, const_cast<char*>(str.data()), str.length());
    }
    template<typename T>
    requires is_plain_type<T>
    T get() {
        constexpr auto len = sizeof(T);
        char buf[len];
        try {
            readfd(fd_, buf, len);
            T* t = reinterpret_cast<T*>(buf);
            return *t;
        } catch (io_exception&) {
            throw;
        }
    }

    template<typename T>
    requires is_plain_type<T>
    void put(const T& t) {
        constexpr auto len = sizeof(T);
        try {
            writefd(fd_, reinterpret_cast<char*>(&t), len);
        } catch (io_exception&) {
            throw;
        }
    }
};