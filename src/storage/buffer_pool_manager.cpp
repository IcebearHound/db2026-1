/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "buffer_pool_manager.h"

bool BufferPoolManager::find_victim_page(frame_id_t* frame_id) {
    if (!free_list_.empty()) {
        *frame_id = free_list_.front();
        free_list_.pop_front();
        return true;
    }
    return replacer_->victim(frame_id);
}

void BufferPoolManager::update_page(Page *page, PageId new_page_id, frame_id_t new_frame_id) {
    if (page->id_.page_no != INVALID_PAGE_ID) {
        if (page->is_dirty_) {
            disk_manager_->write_page(page->id_.fd, page->id_.page_no, page->data_, PAGE_SIZE);
            page->is_dirty_ = false;
        }
        page_table_.erase(page->id_);
    }
    page->reset_memory();
    page->id_ = new_page_id;
    page->pin_count_ = 0;
    page->is_dirty_ = false;
    page_table_[new_page_id] = new_frame_id;
}

Page* BufferPoolManager::fetch_page(PageId page_id) {
    std::scoped_lock lock{latch_};
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        Page *page = &pages_[it->second];
        page->pin_count_++;
        replacer_->pin(it->second);
        return page;
    }

    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        return nullptr;
    }
    Page *page = &pages_[frame_id];
    update_page(page, page_id, frame_id);
    disk_manager_->read_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
    page->pin_count_ = 1;
    return page;
}

bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    std::scoped_lock lock{latch_};
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }
    Page *page = &pages_[it->second];
    if (page->pin_count_ <= 0) {
        return false;
    }
    page->is_dirty_ = page->is_dirty_ || is_dirty;
    page->pin_count_--;
    if (page->pin_count_ == 0) {
        replacer_->unpin(it->second);
    }
    return true;
}

bool BufferPoolManager::flush_page(PageId page_id) {
    std::scoped_lock lock{latch_};
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }
    Page *page = &pages_[it->second];
    disk_manager_->write_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
    page->is_dirty_ = false;
    return true;
}

Page* BufferPoolManager::new_page(PageId* page_id) {
    std::scoped_lock lock{latch_};
    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        return nullptr;
    }
    page_id->page_no = disk_manager_->allocate_page(page_id->fd);
    Page *page = &pages_[frame_id];
    update_page(page, *page_id, frame_id);
    page->pin_count_ = 1;
    page->is_dirty_ = true;
    return page;
}

bool BufferPoolManager::delete_page(PageId page_id) {
    std::scoped_lock lock{latch_};
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return true;
    }
    Page *page = &pages_[it->second];
    if (page->pin_count_ != 0) {
        return false;
    }
    if (page->is_dirty_) {
        disk_manager_->write_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
    }
    frame_id_t frame_id = it->second;
    replacer_->pin(frame_id);
    page_table_.erase(it);
    page->reset_memory();
    page->id_ = PageId{};
    page->id_.page_no = INVALID_PAGE_ID;
    page->pin_count_ = 0;
    page->is_dirty_ = false;
    free_list_.push_back(frame_id);
    return true;
}

void BufferPoolManager::flush_all_pages(int fd) {
    std::scoped_lock lock{latch_};
    for (auto &entry : page_table_) {
        if (entry.first.fd == fd) {
            Page *page = &pages_[entry.second];
            disk_manager_->write_page(entry.first.fd, entry.first.page_no, page->data_, PAGE_SIZE);
            page->is_dirty_ = false;
        }
    }
}
