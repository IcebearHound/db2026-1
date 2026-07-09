/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "lru_replacer.h"

LRUReplacer::LRUReplacer(size_t num_pages) { max_size_ = num_pages; }

LRUReplacer::~LRUReplacer() = default;

bool LRUReplacer::victim(frame_id_t *frame_id) {
    std::scoped_lock lock{latch_};
    if (LRUlist_.empty()) {
        return false;
    }
    frame_id_t victim = LRUlist_.front();
    LRUlist_.pop_front();
    LRUhash_.erase(victim);
    *frame_id = victim;
    return true;
}

void LRUReplacer::pin(frame_id_t frame_id) {
    std::scoped_lock lock{latch_};
    auto it = LRUhash_.find(frame_id);
    if (it == LRUhash_.end()) {
        return;
    }
    LRUlist_.erase(it->second);
    LRUhash_.erase(it);
}

void LRUReplacer::unpin(frame_id_t frame_id) {
    std::scoped_lock lock{latch_};
    if (frame_id < 0 || static_cast<size_t>(frame_id) >= max_size_ || LRUhash_.count(frame_id) != 0 ||
        LRUlist_.size() >= max_size_) {
        return;
    }
    LRUlist_.push_back(frame_id);
    auto it = LRUlist_.end();
    --it;
    LRUhash_[frame_id] = it;
}

size_t LRUReplacer::Size() {
    std::scoped_lock lock{latch_};
    return LRUlist_.size();
}
