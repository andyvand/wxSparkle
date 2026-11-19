/*
 *  This file is part of wxSparkle, a wxWidgets port of WinSparkle
 *  (https://winsparkle.org)
 *
 *  Copyright (C) 2009-2026 Vaclav Slavik
 *  SPDX-License-Identifier: MIT (see COPYING)
 */

#include "threads.h"
#include "utils.h"

#if defined(_WIN32)
    #include <windows.h>
    #include <process.h>
#else
    #include <chrono>
    #include <pthread.h>
    #include <system_error>
#endif

namespace winsparkle
{

/*--------------------------------------------------------------------------*
                                 Helpers
 *--------------------------------------------------------------------------*/

namespace
{

// Sets current thread's name for the debugger
void SetCurrentThreadName(const std::string& name)
{
#if defined(_WIN32)
    // SetThreadDescription() is only available on Windows 10 1607+
    typedef HRESULT (WINAPI *SetThreadDescription_t)(HANDLE, PCWSTR);
    HMODULE kernel = GetModuleHandleA("kernel32.dll");
    if ( kernel )
    {
        auto f = reinterpret_cast<SetThreadDescription_t>(
                     reinterpret_cast<void*>(GetProcAddress(kernel, "SetThreadDescription")));
        if ( f )
            f(GetCurrentThread(), FromUtf8(name).c_str());
    }
#elif defined(__APPLE__)
    pthread_setname_np(name.c_str());
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__)
    // Linux limits thread names to 15 characters + NUL
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#else
    (void)name;
#endif
}

} // anonymous namespace


/*--------------------------------------------------------------------------*
                              Event class
 *--------------------------------------------------------------------------*/

#ifdef _WIN32

Event::Event(bool manualReset)
{
    m_handle = CreateEventW
               (
                   NULL,                        // default security attributes
                   manualReset ? TRUE : FALSE,  // auto-resetting by default
                   FALSE,                       // initially non-signaled
                   NULL                         // anonymous
               );
    if (!m_handle)
        throw Win32Exception();
}

Event::~Event()
{
    CloseHandle(m_handle);
}

void Event::Signal()
{
    SetEvent(m_handle);
}

bool Event::WaitUntilSignaled(unsigned timeoutMilliseconds)
{
    return WaitForSingleObject(m_handle, timeoutMilliseconds) == WAIT_OBJECT_0;
}

#else // !_WIN32

Event::Event(bool manualReset)
    : m_signaled(false), m_manualReset(manualReset)
{
}

Event::~Event()
{
}

void Event::Signal()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_signaled = true;
    }
    if ( m_manualReset )
        m_cond.notify_all();
    else
        m_cond.notify_one();
}

bool Event::WaitUntilSignaled(unsigned timeoutMilliseconds)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if ( timeoutMilliseconds == INFINITE_WAIT )
    {
        m_cond.wait(lock, [this]{ return m_signaled; });
    }
    else if ( !m_cond.wait_for(lock,
                               std::chrono::milliseconds(timeoutMilliseconds),
                               [this]{ return m_signaled; }) )
    {
        return false;
    }

    if ( !m_manualReset )
        m_signaled = false;
    return true;
}

#endif // _WIN32/!_WIN32


/*--------------------------------------------------------------------------*
                              Thread class
 *--------------------------------------------------------------------------*/

Thread::Thread(const char *name)
    : m_name(name ? name : "WinSparkle thread"),
      m_ready(std::make_shared<Event>(true)),
      m_terminate(false),
      m_terminateEvent(true)
#ifdef _WIN32
      , m_handle(NULL), m_id(0)
#endif
{
}


Thread::~Thread()
{
    // A joinable thread must have been joined before destruction; if it
    // wasn't, wait for it here (unless we're the thread itself).
#ifdef _WIN32
    if ( m_handle )
    {
        if ( !IsCurrentThread() )
        {
            RequestTermination();
            WaitForSingleObject(m_handle, INFINITE);
        }
        CloseHandle(m_handle);
    }
#else
    if ( m_thread.joinable() )
    {
        if ( IsCurrentThread() )
            m_thread.detach();
        else
        {
            RequestTermination();
            m_thread.join();
        }
    }
#endif
}


bool Thread::IsCurrentThread() const
{
#ifdef _WIN32
    return m_id != 0 && m_id == GetCurrentThreadId();
#else
    return m_thread.get_id() == std::this_thread::get_id();
#endif
}


/*static*/ void Thread::ThreadEntryPoint(Thread *thread)
{
    SetCurrentThreadName(thread->m_name);

    // must be read before Run(), because a self-destructing thread object
    // may not be accessed after it finished
    const bool joinable = thread->IsJoinable();

    try
    {
        thread->Run();
    }
    catch ( TerminateThreadException& )
    {
        // this is OK, just return
    }
    CATCH_ALL_EXCEPTIONS

    if ( !joinable )
        delete thread;
}


#ifdef _WIN32
/*static*/ unsigned __stdcall Thread::Win32ThreadEntryPoint(void *data)
{
    ThreadEntryPoint(reinterpret_cast<Thread*>(data));
    return 0;
}
#endif


void Thread::Start()
{
    // keep the handshake state alive independently of 'this', which may be
    // deleted by the (self-destructing) thread as soon as it signals readiness
    std::shared_ptr<Event> ready = m_ready;
    const bool joinable = IsJoinable();

#ifdef _WIN32
    unsigned id = 0;
    HANDLE handle = (HANDLE)_beginthreadex
                    (
                        NULL,                      // default security
                        0,                         // default stack size
                        &Thread::Win32ThreadEntryPoint,
                        this,                      // arguments
                        CREATE_SUSPENDED,
                        &id                        // thread ID
                    );
    if ( !handle )
        throw Win32Exception("Failed to start thread");

    if ( joinable )
    {
        m_handle = handle;
        m_id = id;
    }

    if ( ResumeThread(handle) == (DWORD)-1 )
        throw Win32Exception("Failed to start thread");

    // a self-destructing thread's handle isn't needed (and 'this' may be
    // gone already, so only the local copy is used)
    if ( !joinable )
        CloseHandle(handle);
#else
    // Use a local std::thread object: a self-destructing thread may finish
    // (and delete 'this') before we get to detach it.
    std::thread t;
    try
    {
        t = std::thread(&Thread::ThreadEntryPoint, this);
    }
    catch ( const std::system_error& e )
    {
        throw std::runtime_error(std::string("Failed to start thread: ") + e.what());
    }

    if ( joinable )
        m_thread = std::move(t);
    else
        t.detach();
#endif

    // Wait until Run() signals that it is fully initialized.
    // Note that 'this' must not be accessed after this point!
    ready->WaitUntilSignaled();
}


void Thread::Join()
{
    if ( IsCurrentThread() )
        throw std::logic_error("Thread cannot join itself");

#ifdef _WIN32
    if ( !m_handle )
        return;

    if ( WaitForSingleObject(m_handle, INFINITE) != WAIT_OBJECT_0 )
        throw Win32Exception();

    CloseHandle(m_handle);
    m_handle = NULL;
#else
    if ( m_thread.joinable() )
        m_thread.join();
#endif
}


void Thread::RequestTermination()
{
    m_terminate = true;
    m_terminateEvent.Signal();
}


void Thread::TerminateAndJoin()
{
    RequestTermination();
    Join();
}


void Thread::CheckShouldTerminate()
{
    if ( m_terminate.load() )
        throw TerminateThreadException();
}


bool Thread::WaitForTermination(unsigned seconds)
{
    if ( m_terminate.load() )
        return true;

    // avoid overflow of the millisecond count for very long intervals
    const unsigned maxSeconds = (Event::INFINITE_WAIT - 1) / 1000;
    return m_terminateEvent.WaitUntilSignaled((seconds > maxSeconds ? maxSeconds : seconds) * 1000)
           || m_terminate.load();
}


void Thread::SignalReady()
{
    std::shared_ptr<Event> ready = m_ready;
    ready->Signal();
}

} // namespace winsparkle
