#pragma once

#include <memory.h>

#include <functional>
#include <map>

#include "io.h"

template<typename E>
concept is_event = requires (E e) {
    requires std::same_as<decltype(E::unique_event_id), const int>;
    E(general_shared_array_buffer_t(1024, new heap_allocator()));
    { e.content() } -> std::same_as<general_shared_array_buffer_t>;
};

class event_channel {
public:
    using cbid_t = int;
private:
    using evid_t = int;
    using evcallback_t = std::function<void(general_shared_array_buffer_t)>;
    using evhandler_t = struct {
        evcallback_t callback_function;
        cbid_t callback_id;
    };
    std::map<evid_t, std::vector<evhandler_t>> evhandlers_;
    int cbid_count_ {0};
public:
    template<typename E>
    requires is_event<E>
    /* 订阅特定事件，返回event_handler对应的事件处理器句柄，一旦事件总线收到事件则立刻调用event_handler。*/
    cbid_t subscribe(std::function<void(E)>&& event_handler) {
        auto cbid = cbid_count_++;
        evhandler_t packaged = {
            [event_handler](general_shared_array_buffer_t buffer) {
                event_handler(E(std::move(buffer)));
            },
            cbid
        };
        constexpr int ueid = E::unique_event_id;
        if (!evhandlers_.contains(ueid)) {
            evhandlers_[ueid] = std::vector<evhandler_t>();
        }
        evhandlers_[ueid].push_back(std::move(packaged));
        return cbid;
    }

    /* 取消订阅事件 */
    template<typename E>
    requires is_event<E>
    bool unsubscribe(cbid_t callback_id) {
        constexpr int ueid = E::unique_event_id;
        if (evhandlers_.contains(ueid)) {
            auto& handlers = evhandlers_[ueid];
            bool deleted = false;
            handlers.erase(std::remove_if(handlers.begin(), handlers.end(), [&](const evhandler_t& handler) {
                if (handler.callback_id == callback_id) {
                    deleted = true;
                    return true;
                }
                return false;
            }));
            return deleted;
        }
        return false;
    }

    /* 在进程内部(消息总线)广播事件 */
    template<typename E>
    requires is_event<E>
    void post(E event) {
        constexpr int ueid = E::unique_event_id;
        general_shared_array_buffer_t buffer(event.content());
        if (evhandlers_.contains(ueid)) {
            for (auto& handler : evhandlers_[ueid]) {
                handler.callback_function(general_shared_array_buffer_t(buffer));
            }
        }
    }
};

struct event_packet_header {
    int ueid;
    size_t size;
};

template<typename E>
requires is_event<E>
inline void send_packet(int fd, const E& ev) {
    general_shared_array_buffer_t content = ev.content();
    event_packet_header hdr {
        .ueid = E::unique_event_id,
        .size = content.capacity()
    };
    try {
        writefd(fd, reinterpret_cast<char*>(&hdr), sizeof(hdr));
        writefd(fd, content.pointer(), content.capacity());
    } catch (io_exception&) {
        throw;
    }
}

inline auto recv_packet(int fd) {
    nonblocking_socket_stream nbs(fd);
    general_array_buffer_t buffer = nbs.read();
    try {
        buffer_stream stream(buffer);
        auto evid = stream.get_as<int>();
        auto size = stream.get_as<int64_t>();
        general_shared_array_buffer_t recv_buf(size, new heap_allocator());
        buffer_stream recv_stream(recv_buf);
        recv_stream.append(stream.reference(12, size));
        return std::make_tuple(evid, recv_buf);
    } catch (memory_exception& e) {
        e.print();
        throw memory_exception("recv_packet()", "cannot read packet array buffer");
    }
}