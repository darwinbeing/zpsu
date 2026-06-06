#ifndef LED_LED_HPP_
#define LED_LED_HPP_

#include "singleton.hpp"

namespace led {

class Led : public common::Singleton<Led> {
  friend class common::Singleton<Led>;

 public:
  void Start();
  void Stop();

 private:
  Led() = default;
};

}  // namespace led

#endif  // LED_LED_HPP_
