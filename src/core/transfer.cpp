#include "localsend/transfer.hpp"

#include <algorithm>
#include <utility>

namespace localsend {

std::uint64_t TransferStore::add(TransferDirection direction,
                                 std::string file_name,
                                 std::uint64_t size,
                                 std::string peer_alias,
                                 std::string peer_ip) {
  const auto now = std::chrono::system_clock::now();

  std::lock_guard<std::mutex> lock(mutex_);
  TransferItem item;
  item.id = next_id_++;
  item.direction = direction;
  item.status = TransferStatus::Pending;
  item.file_name = std::move(file_name);
  item.size = size;
  item.peer_alias = std::move(peer_alias);
  item.peer_ip = std::move(peer_ip);
  item.created_at = now;
  item.updated_at = now;

  const std::uint64_t id = item.id;
  items_.push_back(std::move(item));
  return id;
}

bool TransferStore::set_status(std::uint64_t id, TransferStatus status) {
  std::lock_guard<std::mutex> lock(mutex_);
  TransferItem* item = find_locked(id);
  if (!item) {
    return false;
  }

  item->status = status;
  if (status == TransferStatus::Completed) {
    item->bytes_transferred = item->size;
    item->error.clear();
  }
  touch_locked(*item);
  return true;
}

bool TransferStore::set_progress(std::uint64_t id, std::uint64_t bytes_transferred) {
  std::lock_guard<std::mutex> lock(mutex_);
  TransferItem* item = find_locked(id);
  if (!item) {
    return false;
  }

  item->bytes_transferred = std::min(bytes_transferred, item->size);
  if (item->status == TransferStatus::Pending || item->status == TransferStatus::Preparing) {
    item->status = TransferStatus::Transferring;
  }
  if (item->size > 0 && item->bytes_transferred == item->size) {
    item->status = TransferStatus::Completed;
    item->error.clear();
  }
  touch_locked(*item);
  return true;
}

bool TransferStore::fail(std::uint64_t id, std::string error) {
  std::lock_guard<std::mutex> lock(mutex_);
  TransferItem* item = find_locked(id);
  if (!item) {
    return false;
  }

  item->status = TransferStatus::Failed;
  item->error = std::move(error);
  touch_locked(*item);
  return true;
}

bool TransferStore::cancel(std::uint64_t id) {
  std::lock_guard<std::mutex> lock(mutex_);
  TransferItem* item = find_locked(id);
  if (!item) {
    return false;
  }

  item->status = TransferStatus::Cancelled;
  touch_locked(*item);
  return true;
}

std::optional<TransferItem> TransferStore::get(std::uint64_t id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const TransferItem* item = find_locked(id);
  if (!item) {
    return std::nullopt;
  }
  return *item;
}

std::vector<TransferItem> TransferStore::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return items_;
}

void TransferStore::clear_completed() {
  std::lock_guard<std::mutex> lock(mutex_);
  items_.erase(std::remove_if(items_.begin(),
                              items_.end(),
                              [](const TransferItem& item) {
                                return item.status == TransferStatus::Completed ||
                                       item.status == TransferStatus::Failed ||
                                       item.status == TransferStatus::Cancelled;
                              }),
               items_.end());
}

void TransferStore::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  items_.clear();
}

TransferItem* TransferStore::find_locked(std::uint64_t id) {
  const auto it = std::find_if(items_.begin(), items_.end(), [id](const TransferItem& item) {
    return item.id == id;
  });
  return it == items_.end() ? nullptr : &*it;
}

const TransferItem* TransferStore::find_locked(std::uint64_t id) const {
  const auto it = std::find_if(items_.begin(), items_.end(), [id](const TransferItem& item) {
    return item.id == id;
  });
  return it == items_.end() ? nullptr : &*it;
}

void TransferStore::touch_locked(TransferItem& item) const {
  item.updated_at = std::chrono::system_clock::now();
}

const char* to_string(TransferDirection direction) {
  switch (direction) {
  case TransferDirection::Send:
    return "send";
  case TransferDirection::Receive:
    return "receive";
  }
  return "receive";
}

const char* to_string(TransferStatus status) {
  switch (status) {
  case TransferStatus::Pending:
    return "pending";
  case TransferStatus::Preparing:
    return "preparing";
  case TransferStatus::Transferring:
    return "transferring";
  case TransferStatus::Completed:
    return "completed";
  case TransferStatus::Failed:
    return "failed";
  case TransferStatus::Cancelled:
    return "cancelled";
  }
  return "pending";
}

} // namespace localsend
