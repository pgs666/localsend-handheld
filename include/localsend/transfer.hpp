#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace localsend {

enum class TransferDirection {
  Send,
  Receive,
};

enum class TransferStatus {
  Pending,
  Preparing,
  Transferring,
  Completed,
  Failed,
  Cancelled,
};

struct TransferItem {
  std::uint64_t id = 0;
  TransferDirection direction = TransferDirection::Receive;
  TransferStatus status = TransferStatus::Pending;
  std::string file_name;
  std::uint64_t size = 0;
  std::uint64_t bytes_transferred = 0;
  std::string peer_alias;
  std::string peer_ip;
  std::string error;
  std::chrono::system_clock::time_point created_at{};
  std::chrono::system_clock::time_point updated_at{};
};

class TransferStore {
public:
  std::uint64_t add(TransferDirection direction,
                    std::string file_name,
                    std::uint64_t size,
                    std::string peer_alias,
                    std::string peer_ip);

  bool set_status(std::uint64_t id, TransferStatus status);
  bool set_progress(std::uint64_t id, std::uint64_t bytes_transferred);
  bool fail(std::uint64_t id, std::string error);
  bool cancel(std::uint64_t id);

  std::optional<TransferItem> get(std::uint64_t id) const;
  std::vector<TransferItem> snapshot() const;
  void clear_completed();
  void clear();

private:
  TransferItem* find_locked(std::uint64_t id);
  const TransferItem* find_locked(std::uint64_t id) const;
  void touch_locked(TransferItem& item) const;

  mutable std::mutex mutex_;
  std::vector<TransferItem> items_;
  std::uint64_t next_id_ = 1;
};

const char* to_string(TransferDirection direction);
const char* to_string(TransferStatus status);

} // namespace localsend
