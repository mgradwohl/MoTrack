#pragma once

#include <type_traits>
#include <utility>
#include <version>

/// `Expected<T, E>` is `std::expected<T, E>` where the standard library provides it. libstdc++ only enables
/// <expected> when `__cpp_concepts >= 202002L`, which Clang 18 (the Linux CI compiler) does not report, so
/// this header supplies the subset MoTrack's OSP layer uses there. Remove the fallback once every supported
/// toolchain provides <expected>.
#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202211L && !defined(MOTRACK_OSP_EXPECTED_FALLBACK)
#include <expected>

namespace motrack::osp
{

template <typename T, typename E> using Expected = std::expected<T, E>;
template <typename E> using Unexpected = std::unexpected<E>;

} // namespace motrack::osp
#else
#include <optional>
#include <variant>

namespace motrack::osp
{

template <typename E> class Unexpected
{
  public:
    constexpr explicit Unexpected(E error) : m_Error(std::move(error))
    {}

    [[nodiscard]] constexpr const E& error() const& noexcept
    {
        return m_Error;
    }

    [[nodiscard]] constexpr E& error() & noexcept
    {
        return m_Error;
    }

    [[nodiscard]] constexpr E&& error() && noexcept
    {
        return std::move(m_Error);
    }

  private:
    E m_Error;
};

template <typename T> inline constexpr bool IS_UNEXPECTED = false;
template <typename E> inline constexpr bool IS_UNEXPECTED<Unexpected<E>> = true;

template <typename T, typename E> class Expected
{
  public:
    constexpr Expected()
        requires std::is_default_constructible_v<T>
        : m_Storage(std::in_place_index<0>)
    {}

    template <typename U = T>
        requires(std::is_constructible_v<T, U &&> && !std::is_same_v<std::remove_cvref_t<U>, Expected> &&
                 !IS_UNEXPECTED<std::remove_cvref_t<U>> && !std::is_same_v<std::remove_cvref_t<U>, std::in_place_t>)
    constexpr Expected(U&& value) : m_Storage(std::in_place_index<0>, std::forward<U>(value))
    {}

    template <typename G> constexpr Expected(const Unexpected<G>& error) : m_Storage(std::in_place_index<1>, error.error())
    {}

    template <typename G> constexpr Expected(Unexpected<G>&& error) : m_Storage(std::in_place_index<1>, std::move(error).error())
    {}

    [[nodiscard]] constexpr bool has_value() const noexcept // NOLINT(readability-identifier-naming): std::expected API
    {
        return m_Storage.index() == 0;
    }

    constexpr explicit operator bool() const noexcept
    {
        return has_value();
    }

    [[nodiscard]] constexpr const T& operator*() const& noexcept
    {
        return *std::get_if<0>(&m_Storage);
    }

    [[nodiscard]] constexpr T& operator*() & noexcept
    {
        return *std::get_if<0>(&m_Storage);
    }

    [[nodiscard]] constexpr T&& operator*() && noexcept
    {
        return std::move(*std::get_if<0>(&m_Storage));
    }

    [[nodiscard]] constexpr const T* operator->() const noexcept
    {
        return std::get_if<0>(&m_Storage);
    }

    [[nodiscard]] constexpr T* operator->() noexcept
    {
        return std::get_if<0>(&m_Storage);
    }

    [[nodiscard]] constexpr const T& value() const&
    {
        return std::get<0>(m_Storage);
    }

    [[nodiscard]] constexpr const E& error() const& noexcept
    {
        return *std::get_if<1>(&m_Storage);
    }

    [[nodiscard]] constexpr E& error() & noexcept
    {
        return *std::get_if<1>(&m_Storage);
    }

    template <typename G>
    [[nodiscard]] constexpr E error_or(G&& fallback) const& // NOLINT(readability-identifier-naming): std::expected API
    {
        return has_value() ? static_cast<E>(std::forward<G>(fallback)) : error();
    }

    template <typename U>
        requires(!std::is_same_v<std::remove_cvref_t<U>, Expected> && !IS_UNEXPECTED<std::remove_cvref_t<U>>)
    friend constexpr bool operator==(const Expected& left, const U& right)
    {
        return left.has_value() && *left == right;
    }

    friend constexpr bool operator==(const Expected& left, const Expected& right)
    {
        return left.m_Storage == right.m_Storage;
    }

  private:
    std::variant<T, E> m_Storage;
};

template <typename E> class Expected<void, E>
{
  public:
    constexpr Expected() noexcept = default;

    template <typename G> constexpr Expected(const Unexpected<G>& error) : m_Error(error.error())
    {}

    template <typename G> constexpr Expected(Unexpected<G>&& error) : m_Error(std::move(error).error())
    {}

    [[nodiscard]] constexpr bool has_value() const noexcept // NOLINT(readability-identifier-naming): std::expected API
    {
        return !m_Error;
    }

    constexpr explicit operator bool() const noexcept
    {
        return has_value();
    }

    [[nodiscard]] constexpr const E& error() const& noexcept
    {
        return *m_Error; // NOLINT(bugprone-unchecked-optional-access): precondition, as for std::expected::error
    }

    [[nodiscard]] constexpr E& error() & noexcept
    {
        return *m_Error; // NOLINT(bugprone-unchecked-optional-access): precondition, as for std::expected::error
    }

  private:
    std::optional<E> m_Error;
};

} // namespace motrack::osp
#endif

namespace motrack::osp
{

/// `std::unexpected(error)` without class template argument deduction through an alias template, which Clang
/// before 19 does not support.
template <typename E> [[nodiscard]] constexpr Unexpected<std::remove_cvref_t<E>> makeUnexpected(E&& error)
{
    return Unexpected<std::remove_cvref_t<E>>(std::forward<E>(error));
}

} // namespace motrack::osp
