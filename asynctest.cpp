// asynctest.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <coroutine>
#include <thread>
#include <queue>
#include <mutex>
#include <functional>
#define WIN32_LEAN_AND_MEAN
#define WIN32_NOMINMAX
#include <Windows.h>
#include <string>
#include "resource.h"

struct task
{
    struct promise_type
    {
        task get_return_object() { return {}; }
        
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() {}
    };
};

struct background_worker
{
    bool canceled{ false };
    std::condition_variable cv;
    std::mutex mtx;
    std::queue<std::coroutine_handle<>> q;
    std::jthread thread;

    void run()
    {
        std::unique_lock lock{ mtx };
        while(!canceled)
        {
            while (q.empty() && ! canceled)
                cv.wait(lock);
            while (!q.empty() && !canceled)
            {
                auto coro{ std::move(q.front()) };
                q.pop();

                lock.unlock();
                coro.resume();
                lock.lock();
            }
        }
    }

    void cancel()
    {
        std::unique_lock lock{ mtx };
        canceled = true;
        cv.notify_one();
    }

    background_worker()
        : thread{ std::bind(&background_worker::run, this) } {}

    ~background_worker()
    {
        cancel();
    }

};
std::optional<background_worker> worker;

auto switch_background()
{
    struct awaitable {
        background_worker& worker;

        bool await_ready() { return false; }

        void await_suspend(std::coroutine_handle<> h)
        {
            std::unique_lock lock{ worker.mtx };
            worker.q.push(std::move(h));
            worker.cv.notify_one();
        }

        void await_resume() {}
    };
    return awaitable(*worker);
}

auto switch_threadpool()
{
    struct awaitable {
        bool await_ready() { return false; }

        static void callback(PTP_CALLBACK_INSTANCE instance, void* ctx, PTP_WORK work)
        {
            auto h{ std::coroutine_handle<>::from_address(ctx) };
            h.resume();
        }

        void await_suspend(std::coroutine_handle<> h)
        {
            auto work{ CreateThreadpoolWork(&awaitable::callback, h.address(), nullptr) };
            SubmitThreadpoolWork(work);
        }

        void await_resume() {}
    };
    return awaitable();
}

task query();
task counter();

HWND dlg_hwnd;

std::mutex foreground_mtx;
std::queue<std::coroutine_handle<>> foreground_ready;
std::queue<std::coroutine_handle<>> timer_ready;
INT_PTR dialog_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
        case WM_INITDIALOG:
            dlg_hwnd = hwnd;
            SetTimer(dlg_hwnd, 0, 1000, nullptr);
            worker.emplace();
            break;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
            case IDC_BUTTON1:
                query();
                break;
            case IDC_BUTTON2:
                counter();
                break;
            case IDOK:
                PostMessage(dlg_hwnd, WM_CLOSE, 0, 0);
                break;
            }
            return 1;
        case WM_TIMER:
        {
            SetTimer(dlg_hwnd, 0, 1000, nullptr);
            std::unique_lock lock{ foreground_mtx };
            while (timer_ready.size())
            {
                auto coro{ std::move(timer_ready.front()) };
                timer_ready.pop();

                lock.unlock();
                coro.resume();
                lock.lock();
            }
            return 1;
        }
        case WM_USER:
        {
            std::unique_lock lock{ foreground_mtx };
            while (foreground_ready.size())
            {
                auto coro{ std::move(foreground_ready.front()) };
                foreground_ready.pop();

                lock.unlock();
                coro.resume();
                lock.lock();
            }
            return 1;
        }
        case WM_CLOSE:
            worker.reset();
            PostQuitMessage(0);
            return 1;
    }

    return 0;
}

auto switch_timer()
{
    struct awaitable {
        HWND hwnd;
        bool await_ready() { return false; }
        
        void await_suspend(std::coroutine_handle<> h)
        {
            std::unique_lock lock{ foreground_mtx };
            timer_ready.push(std::move(h));
        }

        void await_resume() {};
    };

    return awaitable{dlg_hwnd};
}

auto switch_foreground()
{
    struct awaitable {
        HWND hwnd;
        bool await_ready() { return false; }
        
        void await_suspend(std::coroutine_handle<> h)
        {
            std::unique_lock lock{ foreground_mtx };
            foreground_ready.push(std::move(h));
            PostMessage(dlg_hwnd, WM_USER, 0, 0);
        }

        void await_resume() {};
    };

    return awaitable{dlg_hwnd};
}

task query()
{
    auto btn{ GetDlgItem(dlg_hwnd, IDC_BUTTON1) };
    auto lbl{ GetDlgItem(dlg_hwnd, IDC_STATIC1) };

    EnableWindow(btn, 0);
    SetWindowText(lbl, L"Working...");

    co_await switch_threadpool();
    SleepEx(1000, true);

    co_await switch_foreground();
    SetWindowText(lbl, L"Done");
    EnableWindow(btn, true);
}

task counter()
{
    auto btn{ GetDlgItem(dlg_hwnd, IDC_BUTTON2) };
    auto lbl{ GetDlgItem(dlg_hwnd, IDC_STATIC2) };

    query();
    SetWindowText(lbl, L"");
    EnableWindow(btn, 0);
    for (int i = 0; i < 10; ++i)
    {
        co_await switch_threadpool();
//        SleepEx(200, true);

        co_await switch_foreground();
        SetWindowText(lbl, std::to_wstring(i).c_str());
    }
    EnableWindow(btn, true);
}

int main()
{
    DialogBox(nullptr, MAKEINTRESOURCE(IDD_DIALOG1), nullptr, dialog_proc);
}
