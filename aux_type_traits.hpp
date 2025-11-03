#ifndef TYPE_TRAITS_HPP
#define TYPE_TRAITS_HPP

#include <cstddef>

namespace BA_Concurrency {
    template <template <typename> typename T, template <typename> typename U>
    inline constexpr bool is_same_template_v = false;
    template <template <typename> typename T>
    inline constexpr bool is_same_template_v<T, T> = true;

    template <unsigned char Power>
    inline constexpr std::size_t pow2_size = std::size_t{1} << Power;
}

#endif // TYPE_TRAITS_HPP
