#ifndef COMMON_SINGLETON_HPP_
#define COMMON_SINGLETON_HPP_

namespace common {

// CRTP base for Meyers singletons. Provides a single lazily-constructed
// instance and forbids copy/move. Derive like:
//
//   class Foo : public common::Singleton<Foo> {
//     friend class common::Singleton<Foo>;  // lets the base construct Foo
//    public:
//     void Bar();
//    private:
//     Foo() = default;                       // keep the ctor private
//   };
//
// Access the instance via Foo::Instance().
template <typename T>
class Singleton {
 public:
  static T& Instance() {
    static T instance;
    return instance;
  }

  Singleton(const Singleton&) = delete;
  Singleton& operator=(const Singleton&) = delete;
  Singleton(Singleton&&) = delete;
  Singleton& operator=(Singleton&&) = delete;

 protected:
  Singleton() = default;
  ~Singleton() = default;
};

}  // namespace common

#endif  // COMMON_SINGLETON_HPP_
