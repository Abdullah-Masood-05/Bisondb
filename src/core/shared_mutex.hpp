#pragma once

#include <condition_variable>
#include <mutex>

namespace bisondb {

// Writer-preference shared mutex over std::mutex + condition_variable.
// Replaces std::shared_mutex because MinGW's winpthreads rwlock returns
// spurious errors under heavy reader/writer contention (asserting inside
// libstdc++'s lock_shared). Compatible with std::unique_lock and
// std::shared_lock.
class SharedMutex {
  public:
    void lock() {
        std::unique_lock lock(m_);
        ++writersWaiting_;
        cv_.wait(lock, [this] { return !writer_ && readers_ == 0; });
        --writersWaiting_;
        writer_ = true;
    }

    bool try_lock() {
        std::lock_guard lock(m_);
        if (writer_ || readers_ != 0) {
            return false;
        }
        writer_ = true;
        return true;
    }

    void unlock() {
        {
            std::lock_guard lock(m_);
            writer_ = false;
        }
        cv_.notify_all();
    }

    void lock_shared() {
        std::unique_lock lock(m_);
        // Writer preference: pending writers block new readers, so spinning
        // readers cannot starve a writer.
        cv_.wait(lock, [this] { return !writer_ && writersWaiting_ == 0; });
        ++readers_;
    }

    bool try_lock_shared() {
        std::lock_guard lock(m_);
        if (writer_ || writersWaiting_ != 0) {
            return false;
        }
        ++readers_;
        return true;
    }

    void unlock_shared() {
        bool wake = false;
        {
            std::lock_guard lock(m_);
            wake = --readers_ == 0;
        }
        if (wake) {
            cv_.notify_all();
        }
    }

  private:
    std::mutex m_;
    std::condition_variable cv_;
    int readers_ = 0;
    int writersWaiting_ = 0;
    bool writer_ = false;
};

} // namespace bisondb
