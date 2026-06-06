#include "notification_manager.hpp"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>  // MIN, __ASSERT_NO_MSG

#define NOTIFICATION_INVALID_ID    0xFFFFFFFF
#define NOTIFICATION_INVALID_INDEX 0xFFFFFFFF

namespace notify {

void NotificationManager::Init() {
  memset(notifications_, 0, sizeof(notifications_));
  for (int i = 0; i < NOTIFICATION_MANAGER_MAX_STORED; i++) {
    notifications_[i].id = NOTIFICATION_INVALID_ID;
  }
  num_notifications_ = 0;
  active_notification_ = nullptr;
}

int32_t NotificationManager::GetNum() { return num_notifications_; }

not_mngr_notification_t* NotificationManager::Add(ble_comm_notify_t* note) {
  uint32_t idx = FindFreeIdx();
  if (idx == NOTIFICATION_INVALID_INDEX) {
    // List full then we replace the oldest
    idx = FindOldestIdx();
    __ASSERT_NO_MSG(idx != NOTIFICATION_INVALID_INDEX);
    notifications_[idx].id = NOTIFICATION_INVALID_ID;
    num_notifications_--;
  }
  memset(&notifications_[idx], 0, sizeof(not_mngr_notification_t));
  if (strncmp(note->src, "Messenger", note->src_len) == 0) {
    notifications_[idx].src = NOTIFICATION_SRC_MESSENGER;
    notifications_[idx].id = note->id;
    memcpy(notifications_[idx].title, note->title, MIN(note->title_len, NOTIFICATION_MGR_MAX_FIELD_LEN - 1));
    memcpy(notifications_[idx].body, note->body, MIN(note->body_len, NOTIFICATION_MGR_MAX_FIELD_LEN - 1));
    memcpy(notifications_[idx].sender, note->sender, MIN(note->sender_len, NOTIFICATION_MGR_MAX_FIELD_LEN - 1));
  } else if (strncmp(note->src, "Gmail", note->src_len) == 0) {
    // {t:"notify",id:1670967782,src:"Gmail",title:"Jakob Krantz",body:"Nytt test\nDetta YR NÖTT"

    // TODO Subject is before first \n in body so extract that into title field.
    notifications_[idx].src = NOTIFICATION_SRC_GMAIL;
    notifications_[idx].id = note->id;
    memcpy(notifications_[idx].body, note->body, MIN(note->body_len, NOTIFICATION_MGR_MAX_FIELD_LEN - 1));
    memcpy(notifications_[idx].sender, note->title, MIN(note->title_len, NOTIFICATION_MGR_MAX_FIELD_LEN - 1));

    memcpy(notifications_[idx].title, note->title, MIN(note->title_len, NOTIFICATION_MGR_MAX_FIELD_LEN - 1));
  } else {
    // TODO add more
    // For example debug notfication
    // {t:"notify",id:1670967783,src:"Bangle.js Gadgetbridge",subject:"Testar",body:"Testar",sender:"Testar",tel:"Testar"}
    notifications_[idx].src = NOTIFICATION_SRC_NONE;
    notifications_[idx].id = note->id;
    memcpy(notifications_[idx].title, note->src, MIN(note->src_len, NOTIFICATION_MGR_MAX_FIELD_LEN - 1));
    memcpy(notifications_[idx].body, note->body, MIN(note->body_len, NOTIFICATION_MGR_MAX_FIELD_LEN - 1));
    memcpy(notifications_[idx].sender, note->sender, MIN(note->sender_len, NOTIFICATION_MGR_MAX_FIELD_LEN - 1));
  }

  num_notifications_++;

  if (!active_notification_) {
    // Show it!
  }

  return &notifications_[idx];
}

int32_t NotificationManager::Remove(uint32_t id) {
  uint32_t idx = FindIdx(id);
  if (idx != NOTIFICATION_INVALID_INDEX) {
    notifications_[idx].id = NOTIFICATION_INVALID_ID;
    num_notifications_--;
    return 0;
  } else {
    return -ENOENT;
  }
}

int32_t NotificationManager::GetAll(not_mngr_notification_t* nots, int* num_notifications) {
  int num_stored = 0;
  for (int i = 0; i < NOTIFICATION_MANAGER_MAX_STORED; i++) {
    if (notifications_[i].id != NOTIFICATION_INVALID_ID) {
      nots[num_stored] = notifications_[i];
      num_stored++;
    }
  }
  *num_notifications = num_stored;
  return 0;
}

not_mngr_notification_t* NotificationManager::GetNewest() {
  int idx = FindNewestIdx();
  if (idx != NOTIFICATION_INVALID_ID) {
    return &notifications_[idx];
  } else {
    return nullptr;
  }
}

uint32_t NotificationManager::FindIdx(uint32_t id) {
  for (int i = 0; i < NOTIFICATION_MANAGER_MAX_STORED; i++) {
    if (notifications_[i].id == id) {
      return i;
    }
  }
  return NOTIFICATION_INVALID_INDEX;
}

uint32_t NotificationManager::FindFreeIdx() {
  for (int i = 0; i < NOTIFICATION_MANAGER_MAX_STORED; i++) {
    if (notifications_[i].id == NOTIFICATION_INVALID_ID) {
      return i;
    }
  }
  return NOTIFICATION_INVALID_INDEX;
}

uint32_t NotificationManager::FindOldestIdx() {
  uint32_t oldest_idx = NOTIFICATION_INVALID_ID;
  uint32_t oldest_id = NOTIFICATION_INVALID_INDEX;

  for (int i = 0; i < NOTIFICATION_MANAGER_MAX_STORED; i++) {
    if (notifications_[i].id != NOTIFICATION_INVALID_ID && notifications_[i].id < oldest_id) {
      oldest_idx = i;
      oldest_id = notifications_[i].id;
    }
  }

  return oldest_idx;
}

uint32_t NotificationManager::FindNewestIdx() {
  uint32_t newest_idx = NOTIFICATION_INVALID_ID;
  uint32_t newest_id = 0;

  for (int i = 0; i < NOTIFICATION_MANAGER_MAX_STORED; i++) {
    if (notifications_[i].id != NOTIFICATION_INVALID_ID && notifications_[i].id > newest_id) {
      newest_idx = i;
      newest_id = notifications_[i].id;
    }
  }

  return newest_idx;
}

}  // namespace notify
