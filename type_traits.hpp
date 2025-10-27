#ifndef TYPE_TRAITS_HPP
#define TYPE_TRAITS_HPP

namespace BA_Concurrency {
    template <template <typename> typename T, template <typename> typename U>
    inline constexpr bool is_same_template_v = false;
    template <template <typename> typename T>
    inline constexpr bool is_same_template_v<T, T> = true;
} // namespace BA_Concurrency

#endif // TYPE_TRAITS_HPP
