#include "scheduler.h"
#include "log.h"
#include "macro.h"
#include "hook.h"

namespace sylar {

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

static thread_local Scheduler* t_scheduler = nullptr;
static thread_local Fiber* t_scheduler_fiber = nullptr;

Scheduler::Scheduler(size_t threads, bool use_caller, const std::string& name)
    :m_name(name) {
    SYLAR_ASSERT(threads > 0);

    // rootThread表示是否使用主线程作为工作线程
    if(use_caller) {
        sylar::Fiber::GetThis();
        --threads;

        SYLAR_ASSERT(GetThis() == nullptr);
        t_scheduler = this;

        m_rootFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, true));
        sylar::Thread::SetName(m_name);

        t_scheduler_fiber = m_rootFiber.get();
        m_rootThread = sylar::GetThreadId();
        m_threadIds.push_back(m_rootThread);
    } else {
        m_rootThread = -1;
    }
    m_threadCount = threads;
}

Scheduler::~Scheduler() {
    SYLAR_ASSERT(m_stopping);
    if(GetThis() == this) {
        t_scheduler = nullptr;
    }
}

Scheduler* Scheduler::GetThis() {
    return t_scheduler;
}

Fiber* Scheduler::GetMainFiber() {
    return t_scheduler_fiber;
}

void Scheduler::start() {
    MutexType::Lock lock(m_mutex);
    if(!m_stopping) {
        return;
    }
    m_stopping = false;
    SYLAR_ASSERT(m_threads.empty());

    m_threads.resize(m_threadCount);
    for(size_t i = 0; i < m_threadCount; ++i) {
        // 线程入口函数Scheduler::run
        m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this)
                            , m_name + "_" + std::to_string(i)));
        m_threadIds.push_back(m_threads[i]->getId());
    }
    lock.unlock();
}

void Scheduler::stop() {
    m_autoStop = true;
    if(m_rootFiber
            && m_threadCount == 0
            && (m_rootFiber->getState() == Fiber::TERM
                || m_rootFiber->getState() == Fiber::INIT)) {
        SYLAR_LOG_INFO(g_logger) << this << " stopped";
        m_stopping = true;

        if(stopping()) {
            return;
        }
    }

    //bool exit_on_this_fiber = false;
    if(m_rootThread != -1) {
        SYLAR_ASSERT(GetThis() == this);
    } else {
        SYLAR_ASSERT(GetThis() != this);
    }

    m_stopping = true;
    for(size_t i = 0; i < m_threadCount; ++i) {
        tickle();
    }

    if(m_rootFiber) {
        tickle();
    }

    if(m_rootFiber) {
        //while(!stopping()) {
        //    if(m_rootFiber->getState() == Fiber::TERM
        //            || m_rootFiber->getState() == Fiber::EXCEPT) {
        //        m_rootFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, true));
        //        SYLAR_LOG_INFO(g_logger) << " root fiber is term, reset";
        //        t_fiber = m_rootFiber.get();
        //    }
        //    m_rootFiber->call();
        //}
        if(!stopping()) {
            m_rootFiber->call();
        }
    }

    std::vector<Thread::ptr> thrs;
    {
        MutexType::Lock lock(m_mutex);
        thrs.swap(m_threads);
    }

    for(auto& i : thrs) {
        i->join();
    }
    //if(exit_on_this_fiber) {
    //}
}

void Scheduler::setThis() {
    t_scheduler = this;
}

void Scheduler::run() {
    SYLAR_LOG_DEBUG(g_logger) << m_name << " run";
    set_hook_enable(true);
    setThis();// 这是当前进入这个函数的线程设置到t_scheduler，记录每个线程调度器对象是谁
    if(sylar::GetThreadId() != m_rootThread) {
        t_scheduler_fiber = Fiber::GetThis().get();     // 非use_caller模式，线程进入这里，都会创建各自的主协程作为调度协程
    }

    Fiber::ptr idle_fiber(new Fiber(std::bind(&Scheduler::idle, this)));
    Fiber::ptr cb_fiber;

    FiberAndThread ft;
    while(true) {
        ft.reset();
        bool tickle_me = false;
        bool is_active = false;
        {
            MutexType::Lock lock(m_mutex);
            auto it = m_fibers.begin();
            // while循环找到一个当前线程能够执行的任务ft
            while(it != m_fibers.end()) {
                // thread == -1 表示协程任务有指定的线程去做，不是当前线程要做的
                if(it->thread != -1 && it->thread != sylar::GetThreadId()) {
                    ++it;
                    tickle_me = true;   // 标记一下：我虽然干不了这个活，但队列里还有活，一会记得叫醒其他睡觉的兄弟来看看是不是他们的活
                    continue;
                }

                SYLAR_ASSERT(it->fiber || it->cb);
                // 当前线程能够执行此任务，如果正在执行态，那就不需要重新分配协程执行了
                if(it->fiber && it->fiber->getState() == Fiber::EXEC) {
                    ++it;
                    continue;
                }

                ft = *it;
                m_fibers.erase(it++);
                ++m_activeThreadCount;
                is_active = true;
                break;
            }
            tickle_me |= it != m_fibers.end();
        }

        if(tickle_me) {
            tickle();
        }
        // ft.fiber表示已经是一个创建好的fiber对象
        if(ft.fiber && (ft.fiber->getState() != Fiber::TERM
                        && ft.fiber->getState() != Fiber::EXCEPT)) {
            // 切换到fiber执行，等到此fiber切换出去后，才会进行下一步--m_activeThreadCount;
            ft.fiber->swapIn();
            --m_activeThreadCount;

            if(ft.fiber->getState() == Fiber::READY) {
                // 3.1 如果状态是 READY，说明协程是主动 YieldToReady 的，它还想继续执行。
                // 把他重新塞回任务队列排队。
                schedule(ft.fiber);
            } else if(ft.fiber->getState() != Fiber::TERM
                    && ft.fiber->getState() != Fiber::EXCEPT) {
                ft.fiber->m_state = Fiber::HOLD;
            }
            ft.reset();
        } else if(ft.cb) {
            if(cb_fiber) {
                // 如果当前线程手里刚好有一个闲置的协程对象 (cb_fiber)，就直接复用它！
                // reset 会把新的函数绑定上去，状态重置为 INIT，但【不会重新分配栈内存】。
                cb_fiber->reset(ft.cb);
            } else {
                cb_fiber.reset(new Fiber(ft.cb));
            }
            ft.reset();
            // 3. 切换到这个协程执行
            cb_fiber->swapIn();
            // Fiber里面的任务执行结束后，会swapOut，里面目标协程的上下文就是t_scheduler_fiber的上下文，回到下一行执行
            --m_activeThreadCount;
            if(cb_fiber->getState() == Fiber::READY) {
                schedule(cb_fiber);
                cb_fiber.reset();
            } else if(cb_fiber->getState() == Fiber::EXCEPT
                    || cb_fiber->getState() == Fiber::TERM) {
                cb_fiber->reset(nullptr);
            } else {//if(cb_fiber->getState() != Fiber::TERM) {
                cb_fiber->m_state = Fiber::HOLD;
                cb_fiber.reset();
            }
        } else { // 当前线程没活干了！这里会各自创建一个idle协程
            // 1. 这是一个防御性判断。如果刚才其实是取到了任务并执行了，只是恰好走到了 else，就 continue 继续下一次取任务。
            if(is_active) {
                --m_activeThreadCount;
                continue;
            }
            if(idle_fiber->getState() == Fiber::TERM) {
                SYLAR_LOG_INFO(g_logger) << "idle fiber term";
                break;
            }
            // 3. 线程进入空闲状态
            ++m_idleThreadCount;
            // 4. 从调度协程切换到 idle 协程执行！线程陷入内核态睡眠
            // 在基础 Scheduler 中，idle() 会不断 YieldToHold，YieldToHold里面是swapOut，又被切换回来了，所以空闲等待任务的线程不断的在这一步swapIn和swapOut
            // 在 IOManager 中，idle() 会在这里陷入 epoll_wait 睡眠，直到有网络事件或被 tickle 唤醒。
            idle_fiber->swapIn();
            --m_idleThreadCount;
            if(idle_fiber->getState() != Fiber::TERM
                    && idle_fiber->getState() != Fiber::EXCEPT) {
                idle_fiber->m_state = Fiber::HOLD;
            }
        }
    }
}

void Scheduler::tickle() {
    SYLAR_LOG_INFO(g_logger) << "tickle";
}

bool Scheduler::stopping() {
    MutexType::Lock lock(m_mutex);
    return m_autoStop && m_stopping
        && m_fibers.empty() && m_activeThreadCount == 0;
}

void Scheduler::idle() {
    SYLAR_LOG_INFO(g_logger) << "idle";
    while(!stopping()) {
        sylar::Fiber::YieldToHold();
    }
}

void Scheduler::switchTo(int thread) {
    SYLAR_ASSERT(Scheduler::GetThis() != nullptr);
    if(Scheduler::GetThis() == this) {
        if(thread == -1 || thread == sylar::GetThreadId()) {
            return;
        }
    }
    schedule(Fiber::GetThis(), thread);
    Fiber::YieldToHold();
}

std::ostream& Scheduler::dump(std::ostream& os) {
    os << "[Scheduler name=" << m_name
       << " size=" << m_threadCount
       << " active_count=" << m_activeThreadCount
       << " idle_count=" << m_idleThreadCount
       << " stopping=" << m_stopping
       << " ]" << std::endl << "    ";
    for(size_t i = 0; i < m_threadIds.size(); ++i) {
        if(i) {
            os << ", ";
        }
        os << m_threadIds[i];
    }
    return os;
}

SchedulerSwitcher::SchedulerSwitcher(Scheduler* target) {
    m_caller = Scheduler::GetThis();
    if(target) {
        target->switchTo();
    }
}

SchedulerSwitcher::~SchedulerSwitcher() {
    if(m_caller) {
        m_caller->switchTo();
    }
}

}
