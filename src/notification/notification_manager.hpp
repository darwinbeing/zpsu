#ifndef NOTIFICATION_NOTIFICATION_MANAGER_HPP_
#define NOTIFICATION_NOTIFICATION_MANAGER_HPP_

#include <inttypes.h>

#include <ble_comm.h>

#include "singleton.hpp"

#define NOTIFICATION_MGR_MAX_FIELD_LEN  50
#define NOTIFICATION_MANAGER_MAX_STORED 5

typedef enum notification_src {
    NOTIFICATION_SRC_MESSENGER,
    NOTIFICATION_SRC_GMAIL,
    NOTIFICATION_SRC_NONE
} notification_src_t;

typedef struct not_mngr_notification {
    uint32_t id;
    char sender[NOTIFICATION_MGR_MAX_FIELD_LEN];
    char title[NOTIFICATION_MGR_MAX_FIELD_LEN];
    char body[NOTIFICATION_MGR_MAX_FIELD_LEN];
    notification_src_t src;
} not_mngr_notification_t;

namespace notify {

class NotificationManager : public common::Singleton<NotificationManager> {
  friend class common::Singleton<NotificationManager>;

 public:
  void Init();
  not_mngr_notification_t* Add(ble_comm_notify_t* notification);
  int32_t Remove(uint32_t id);
  int32_t GetAll(not_mngr_notification_t* notifications, int* num_notifications);
  int32_t GetNum();
  not_mngr_notification_t* GetNewest();

 private:
  NotificationManager() = default;

  uint32_t FindIdx(uint32_t id);
  uint32_t FindFreeIdx();
  uint32_t FindOldestIdx();
  uint32_t FindNewestIdx();

  not_mngr_notification_t notifications_[NOTIFICATION_MANAGER_MAX_STORED];
  uint8_t num_notifications_ = 0;
  not_mngr_notification_t* active_notification_ = nullptr;
};

}  // namespace notify

#endif  // NOTIFICATION_NOTIFICATION_MANAGER_HPP_
