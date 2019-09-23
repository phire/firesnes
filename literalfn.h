#include <type_traits>
#include <utility>

template<class ReturnType, class...Xs>
struct CallableBase {
    using fnType = ReturnType(*)(Xs...);
    virtual ReturnType operator()(Xs...) const = 0;
    virtual ReturnType operator()(Xs...) = 0;
    virtual void copy(void*) const = 0;
    virtual fnType ptr() = 0;
};



template<class F, class ReturnType, class...Xs>
struct Callable final
: CallableBase<ReturnType, Xs...>
{
    F f;

    Callable(F const& f) : f(f) {}

    virtual void copy(void* memory) const {
        new (memory) Callable<F, ReturnType, Xs...>(f);
    }

    virtual ReturnType operator()(Xs... xs) const {
        return f(xs...);
    }

    virtual ReturnType operator()(Xs... xs) {
        return f(xs...);
    }

    using fnType = ReturnType(*)(Xs...);
    virtual fnType ptr() {
        return &F::operator();
    }
};

template<class F, class ReturnType, class...Xs>
struct CallablePtr final
: CallableBase<ReturnType, Xs...>
{
    F *f;

    CallablePtr(F const* f) : f(f) {}

    virtual void copy(void* memory) const {
        new (memory) CallablePtr<F, ReturnType, Xs...>(f);
    }

    virtual ReturnType operator()(Xs... xs) const {
        return f(xs...);
    }

    virtual ReturnType operator()(Xs... xs) {
        return f(xs...);
    }

    using fnType = ReturnType(*)(Xs...);
    virtual fnType ptr() {
        return f;
    }
};



template<class Signature, unsigned size=128>
class LiteralFn;


template<class ReturnType, class...Xs, unsigned size>
class LiteralFn<ReturnType(Xs...), size> {
    char memory[size];
    bool allocated = false;

    using Base = CallableBase<ReturnType, Xs...>;

public:
    constexpr LiteralFn(){}

    template<class F>
    constexpr LiteralFn(F const&f) {
        if constexpr (std::is_function<F>::value) {
            static_assert(sizeof(CallablePtr<F, ReturnType, Xs...>) <= sizeof(memory));
            new (memory) CallablePtr<F, ReturnType, Xs...>(f);
        } else {
            static_assert(sizeof(Callable<F, ReturnType, Xs...>) <= sizeof(memory));
            new (memory) Callable<F, ReturnType, Xs...>(f);
        }
        allocated = true;
    }

    template<class...Ys>
    constexpr ReturnType operator()(Ys&&...ys) {
        return (*reinterpret_cast<Base*>(memory))(std::forward<Ys>(ys)...);
    }

    template<class...Ys>
    constexpr ReturnType operator()(Ys&&...ys)const {
        return *reinterpret_cast<Base*>(memory)(std::forward<Ys>(ys)...);
    }

    using fnType = ReturnType(*)(Xs...);
    virtual fnType ptr() {
        return *reinterpret_cast<Base*>(memory)->ptr();
    }
};