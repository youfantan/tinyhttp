#pragma once

#include <evchannel.h>

#include <functional>
#include <thread>
#include <unordered_map>

class tick_event {
private:
    general_shared_array_buffer_t buffer_;
    int64_t ticks_;
public:
    constexpr static int unique_event_id = 1;
    explicit tick_event(int64_t ticks) : buffer_(8, new heap_allocator()), ticks_(ticks) {
        buffer_stream stream(buffer_);
        stream.append(ticks);
    }
    explicit tick_event(general_shared_array_buffer_t buffer) : buffer_(std::move(buffer)) {
        buffer_stream stream(buffer_);
        ticks_ = stream.get_as<int64_t>();
    }
    general_shared_array_buffer_t content() {return buffer_;}
    [[nodiscard]] int64_t get_ticks() const {
        return ticks_;
    }
};

/* 计时器工具，提供了20Hz(50ms/pertick)精度的定时任务调度， */
class timer {
public:
    using tv_t = struct {
        int64_t gap;
        int64_t countdown;
        int64_t spend;
        int64_t total;
    };

    static tv_t make_tv(int64_t gap, int64_t times) {
        return { gap, gap, 0, times };
    }

    constexpr static tv_t invalid = {-1, -1, -1, -1};
    constexpr static int64_t inf_times = -1;
    constexpr static int sec = 20;
    constexpr static int min = sec * 60;
    constexpr static int hr = min * 60;

    using callback_id_t = int;
    using callback_t = std::function<void(callback_id_t, tv_t)>;
private:
    using task_t = struct {
        tv_t tv;
        callback_t cb;
    };
    std::unordered_map<callback_id_t, task_t> tasks_;
    int cid_count_ {0};
    std::mutex mtx_;
    bool flag_ {false};

public:
    int add(tv_t tv, callback_t&& cb) {
        std::lock_guard lock(mtx_);
        auto cid = cid_count_++;
        tasks_[cid] = {tv, cb};
        return cid;
    }

    bool cancel(callback_id_t cid) {
        std::lock_guard lock(mtx_);
        if (tasks_.contains(cid)) {
            tasks_.erase(cid);
            return true;
        }
        return false;
    }

    tv_t query(callback_id_t cid) {
        if (tasks_.contains(cid)) {
            return tasks_[cid].tv;
        }
        return {-1, -1, -1, -1};
    }

    void run(event_channel &channel) {
        channel.subscribe<tick_event>([this](const tick_event&) {
            std::lock_guard lock(mtx_);
            for (auto &task: tasks_) {
                auto cid = task.first;
                auto& [tv, cb] = task.second;
                if (--tv.countdown == 0) {
                    cb(cid, tv);
                    if (++tv.spend == tv.total) {
                        tasks_.erase(cid);
                        continue;
                    }
                    tv.countdown = tv.gap;
                }
            }
        });
    }
    void stop() {
        flag_ = false;
    }
};
