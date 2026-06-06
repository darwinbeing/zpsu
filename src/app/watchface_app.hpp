#ifndef APP_WATCHFACE_APP_HPP_
#define APP_WATCHFACE_APP_HPP_

#include <cstdint>

#include <lvgl.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/bluetooth/conn.h>

namespace app {

// Watchface coordinator: zbus listeners, BT connection callbacks, and the
// periodic work that drives the watchface UI.
class WatchfaceApp {
 public:
  static WatchfaceApp& Instance();

  void Init();                  // SYS_INIT: init work items, running_ = false
  void Start(lv_group_t* group);
  void Stop();

  bool running() const { return running_; }
  void set_running(bool value) { running_ = value; }

  // Forwarded from the C glue (zbus listeners / BT callbacks).
  void HandleBleComm(const struct zbus_channel* chan);
  void HandleAccel(const struct zbus_channel* chan);
  void HandleChg(const struct zbus_channel* chan);
  void HandlePsu(const struct zbus_channel* chan);
  void HandleConnected(struct bt_conn* conn, uint8_t err);
  void HandleDisconnected(struct bt_conn* conn, uint8_t reason);

 private:
  WatchfaceApp() = default;
  WatchfaceApp(const WatchfaceApp&) = delete;
  WatchfaceApp& operator=(const WatchfaceApp&) = delete;
  WatchfaceApp(WatchfaceApp&&) = delete;
  WatchfaceApp& operator=(WatchfaceApp&&) = delete;

  bool running_ = false;
};

}  // namespace app

#endif  // APP_WATCHFACE_APP_HPP_
