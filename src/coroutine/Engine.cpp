#include <afina/coroutine/Engine.h>

#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace Afina {
namespace Coroutine {

void Engine::Store(context &ctx) {
    volatile char curr_stack_ptr;

    std::tie(ctx.Low, ctx.Hight) = std::minmax(const_cast<char *>(&curr_stack_ptr), StackBottom);

    std::ptrdiff_t curr_stack_size = ctx.Hight - ctx.Low;

    if(std::get<1>(ctx.Stack) < curr_stack_size){
        delete[] std::get<0>(ctx.Stack);
        std::get<0>(ctx.Stack) = new char[curr_stack_size];
        std::get<1>(ctx.Stack) = curr_stack_size;
    }

    std::memcpy(std::get<0>(ctx.Stack), ctx.Low, static_cast<size_t>(curr_stack_size));
}

void Engine::Restore(context &ctx) {
    volatile char curr_stack_ptr;

    if(ctx.Low <= &curr_stack_ptr && &curr_stack_ptr <= ctx.Hight){
        Restore(ctx);
    }

    std::memcpy(ctx.Low, std::get<0>(ctx.Stack), ctx.Hight - ctx.Low);
    std::longjmp(ctx.Environment, 1);
}

void Engine::yield() {
    context *routine_iter = alive;

    while(routine_iter && routine_iter == cur_routine){
        routine_iter = routine_iter->next;
    }

    if(routine_iter){
        sched(routine_iter);
    }
}

void Engine::sched(void *routine_) {
    auto new_routine = static_cast<context *>(routine_);

    if(!routine_ || routine_ == cur_routine){
        return;
    }

    if(cur_routine) {
        Store(*cur_routine);
        if (setjmp(cur_routine->Environment)) {
            return;
        }
    }
    cur_routine = new_routine;
    Restore(*new_routine);
}


} // namespace Coroutine
} // namespace Afina
