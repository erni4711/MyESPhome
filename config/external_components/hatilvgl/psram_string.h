#pragma once

#include <esp_heap_caps.h>

#include <cstddef>
#include <string>

namespace web_admin_local {

template <typename T>
struct PsramAllocator {
  using value_type = T;

  T *allocate(std::size_t count) {
    return static_cast<T *>(heap_caps_malloc(
        count * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }

  void deallocate(T *pointer, std::size_t) { heap_caps_free(pointer); }

  template <typename U>
  bool operator==(const PsramAllocator<U> &) const {
    return true;
  }

  template <typename U>
  bool operator!=(const PsramAllocator<U> &) const {
    return false;
  }
};

using PsramString =
    std::basic_string<char, std::char_traits<char>, PsramAllocator<char>>;

}  // namespace web_admin_local
