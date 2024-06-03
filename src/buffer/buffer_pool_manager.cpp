#include "buffer/buffer_pool_manager.h"
#include <cstddef>

#include "common/config.h"
#include "glog/logging.h"
#include "page/bitmap_page.h"

static const char EMPTY_PAGE_DATA[PAGE_SIZE] = {0};

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager *disk_manager)
    : pool_size_(pool_size), disk_manager_(disk_manager) {
  pages_ = new Page[pool_size_];
  replacer_ = new LRUReplacer(pool_size_);
  for (size_t i = 0; i < pool_size_; i++) {
    free_list_.emplace_back(i);
  }
}

BufferPoolManager::~BufferPoolManager() {
  for (auto page : page_table_) {
    FlushPage(page.first);
  }
  delete[] pages_;
  delete replacer_;
}

/**
 * TODO: Student Implement
 */
Page *BufferPoolManager::FetchPage(page_id_t page_id) {
  auto iter = page_table_.find(page_id);
  if (page_table_.find(page_id) != page_table_.end())  // 该页存在于内存中
  {
    frame_id_t frame_id = iter->second;
    replacer_->Pin(frame_id);  // pin这一页，增加访问次数一次
    pages_[frame_id].pin_count_++;
    return &pages_[frame_id];  // 返回对应页指针
  }

  if (free_list_.empty())  // 内存池中没有空页
  {
    frame_id_t frame_id;
    replacer_->Victim(&frame_id);  // 获取一个可替换frame_id
    if (frame_id == INVALID_FRAME_ID) return nullptr;
    page_id_t replace_page_id = pages_[frame_id].GetPageId();  // 获取替换页的逻辑页号
    if (pages_[frame_id].IsDirty()) {                          // 脏页，重新写回磁盘
      disk_manager_->WritePage(replace_page_id, pages_[frame_id].GetData());
    }
    iter = page_table_.find(replace_page_id);
    page_table_.erase(iter);

    replacer_->Unpin(frame_id);                                    // 解除空页绑定
    disk_manager_->ReadPage(page_id, pages_[frame_id].GetData());  // 从硬盘中读取
    pages_[frame_id].page_id_ = page_id;
    pages_[frame_id].is_dirty_ = false;
    pages_[frame_id].pin_count_ = 1;
    replacer_->Pin(frame_id);
    page_table_.insert({page_id, frame_id});  // 向page_table插入对应关系
    return &pages_[frame_id];
  } else  // 内存中有空页
  {
    frame_id_t frame_id = free_list_.front();  // 获取第一个空页的页帧
    free_list_.erase(free_list_.begin());      // 从链表中删除第一个空页
    replacer_->Unpin(frame_id);
    disk_manager_->ReadPage(page_id, pages_[frame_id].GetData());  // 从硬盘中读取数据
    pages_[frame_id].page_id_ = page_id;
    pages_[frame_id].is_dirty_ = false;
    pages_[frame_id].pin_count_ = 1;
    replacer_->Pin(frame_id);  // 固定当前页，有访问
    page_table_.insert({page_id, frame_id});
    return &pages_[frame_id];
  }
  return nullptr;
}

/**
 * TODO: Student Implement
 */
Page *BufferPoolManager::NewPage(page_id_t &page_id) {
  if (!free_list_.empty())  // 缓冲区有空闲页
  {
    frame_id_t frame_id = free_list_.front();  // 获取第一个空页
    free_list_.erase(free_list_.begin());      // 开删
    page_id = AllocatePage();                  // 从磁盘中读取一个空页（逻辑号）
    if (page_id == INVALID_PAGE_ID) return nullptr;

    pages_[frame_id].ResetMemory();  // 更新page的信息，注意空页固定不能解除
    pages_[frame_id].page_id_ = page_id;
    pages_[frame_id].is_dirty_ = false;
    pages_[frame_id].pin_count_ = 0;
    page_table_.insert({page_id, frame_id});
    return &pages_[frame_id];
  } else {
    // 没有空闲页
    frame_id_t frame_id;
    replacer_->Victim(&frame_id);  // 根据replacer策略获得一个替换的页R
    if (frame_id == INVALID_FRAME_ID) return nullptr;
    page_id = AllocatePage();
    if (page_id == INVALID_PAGE_ID) return nullptr;
    page_id_t replace_page_id = pages_[frame_id].GetPageId();
    if (pages_[frame_id].IsDirty()) {
      disk_manager_->WritePage(replace_page_id, pages_[frame_id].GetData());
    }
    auto iter = page_table_.find(replace_page_id);
    page_table_.erase(iter);
    pages_[frame_id].page_id_ == page_id;
    pages_[frame_id].is_dirty_ = false;
    pages_[frame_id].pin_count_ = 0;
    pages_[frame_id].ResetMemory();
    page_table_.insert({page_id, frame_id});
    return &pages_[frame_id];
  }
  return nullptr;
}

/**
 * TODO: Student Implement
 */
bool BufferPoolManager::DeletePage(page_id_t page_id) {
  auto iter = page_table_.find(page_id);
  if (iter != page_table_.end())  // 该页存在在缓冲区
  {
    frame_id_t frame_id = page_table_[page_id];
    if (pages_[frame_id].GetPinCount() > 0)  // 该页还在使用
    {
      return false;
    }
    replacer_->Reset(frame_id);  // replacer中将该页清除
    page_table_.erase(iter);
    free_list_.emplace_back(frame_id);
    pages_[frame_id].ResetMemory();  // 重置page信息
    pages_[frame_id].is_dirty_ = false;
    pages_[frame_id].pin_count_ = 0;
    pages_[frame_id].page_id_ = INVALID_PAGE_ID;
    DeallocatePage(page_id);  // 从磁盘中释放该页
    return true;
  }
  return false;
}

/**
 * TODO: Student Implement
 */
bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
  frame_id_t frame_id = page_table_[page_id];
  replacer_->Unpin(frame_id);
  pages_[frame_id].is_dirty_ = is_dirty;
  return true;
}

/**
 * TODO: Student Implement
 */
bool BufferPoolManager::FlushPage(page_id_t page_id) {
  // 将内存中的所有页写回磁盘
  frame_id_t frame_id = page_table_[page_id];
  disk_manager_->WritePage(page_id, pages_[frame_id].GetData());
  return true;
}

page_id_t BufferPoolManager::AllocatePage() {
  int next_page_id = disk_manager_->AllocatePage();
  return next_page_id;
}

void BufferPoolManager::DeallocatePage(__attribute__((unused)) page_id_t page_id) {
  disk_manager_->DeAllocatePage(page_id);
}

bool BufferPoolManager::IsPageFree(page_id_t page_id) { return disk_manager_->IsPageFree(page_id); }

// Only used for debug
bool BufferPoolManager::CheckAllUnpinned() {
  bool res = true;
  for (size_t i = 0; i < pool_size_; i++) {
    if (pages_[i].pin_count_ != 0) {
      res = false;
      LOG(ERROR) << "page " << pages_[i].page_id_ << " pin count:" << pages_[i].pin_count_ << endl;
    }
  }
  return res;
}