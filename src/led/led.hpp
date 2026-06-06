#ifndef LED_LED_HPP_
#define LED_LED_HPP_

namespace led {

class Led {
 public:
  static Led& Instance();
  void Start();
  void Stop();

 private:
  Led() = default;
  Led(const Led&) = delete;
  Led& operator=(const Led&) = delete;
  Led(Led&&) = delete;
  Led& operator=(Led&&) = delete;
};

}  // namespace led

#endif  // LED_LED_HPP_
