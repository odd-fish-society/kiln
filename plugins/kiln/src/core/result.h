#pragma once

#include <optional>
#include <string>
#include <utility>

namespace kiln {

template <class T>
struct Result {
    std::optional<T> value;
    std::string error;

    bool ok() const { return value.has_value(); }

    T& operator*() { return *value; }
    const T& operator*() const { return *value; }
    T* operator->() { return &*value; }
    const T* operator->() const { return &*value; }

    static Result<T> success(T v) {
        Result<T> r;
        r.value = std::move(v);
        return r;
    }
    static Result<T> fail(std::string e) {
        Result<T> r;
        r.error = std::move(e);
        return r;
    }
};

template <>
struct Result<void> {
    std::string error;

    bool ok() const { return error.empty(); }

    static Result<void> success() { return {}; }
    static Result<void> fail(std::string e) {
        Result<void> r;
        r.error = std::move(e);
        return r;
    }
};

// Converts any Result<T> into Result<void> (for KILN_TRY in void functions).
template <class T>
Result<void> void_result(Result<T> r) {
    if (!r.ok())
        return Result<void>::fail(std::move(r.error));
    return Result<void>::success();
}

} // namespace kiln

#define KILN_TRY(res)                                                       \
    do {                                                                    \
        auto _kiln_try_res = (res);                                         \
        if (!_kiln_try_res.ok())                                            \
            return decltype(_kiln_try_res)::fail(std::move(_kiln_try_res.error)); \
    } while (0)

#define KILN_TRY_ASSIGN(var, res)                                           \
    auto var##_res = (res);                                                 \
    if (!var##_res.ok())                                                    \
        return decltype(var##_res)::fail(std::move(var##_res.error));       \
    auto var = std::move(*var##_res)
