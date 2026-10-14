#pragma once
#include <exception>

namespace tatsu::detail
{
// PDF4QT runs AfterOperation from a noexcept scope-guard destructor.
// Preserve the first failure until a normal hook or the caller's finish check.
class ContentCallbackGuard
{
public:
    template <typename Function> void invoke(bool after, Function function)
    {
        if (!after)
        {
            check();
            function();
            return;
        }
        if (std::uncaught_exceptions())
            return;
        try
        {
            function();
        }
        catch (...)
        {
            if (!error)
                error = std::current_exception();
        }
    }
    void check() const
    {
        if (error)
            std::rethrow_exception(error);
    }

private:
    std::exception_ptr error;
};
} // namespace tatsu::detail
