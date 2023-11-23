// asynctest.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <coroutine>
#include <thread>
#include <queue>
#include <mutex>
#include <functional>
#include <future>
#define WIN32_LEAN_AND_MEAN
#define WIN32_NOMINMAX
#include <Windows.h>
#include <string>
#include "resource.h"

struct promise_t;

using coroutine_t = std::coroutine_handle<promise_t>;

template<>
struct std::coroutine_traits<coroutine_t> {
    using promise_type = promise_t;
};

struct promise_t
{
    PAINTSTRUCT* ps{};

    coroutine_t get_return_object() { return { coroutine_t::from_promise(*this) }; }
    
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {}
};

struct background_worker
{
    bool canceled{ false };
    std::condition_variable cv;
    std::mutex mtx;
    std::queue<coroutine_t> q;
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

        void await_suspend(coroutine_t h)
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
            auto h{ coroutine_t::from_address(ctx) };
            h.resume();
        }

        void await_suspend(coroutine_t h)
        {
            auto work{ CreateThreadpoolWork(&awaitable::callback, h.address(), nullptr) };
            SubmitThreadpoolWork(work);
        }

        void await_resume() {}
    };
    return awaitable();
}

coroutine_t query();
coroutine_t counter();
coroutine_t blink();

HWND dlg_hwnd;

std::mutex foreground_mtx;
std::queue<coroutine_t> foreground_ready;
std::queue<coroutine_t> timer_ready;
std::queue<coroutine_t> paint_ready;
INT_PTR dialog_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
        case WM_INITDIALOG:
            dlg_hwnd = hwnd;
            SetTimer(dlg_hwnd, 0, 1000, nullptr);
            worker.emplace();

            blink();
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
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(dlg_hwnd, &ps);
            std::unique_lock lock{ foreground_mtx };
            while (paint_ready.size())
            {
                auto coro{ std::move(paint_ready.front()) };
                paint_ready.pop();

                lock.unlock();
                coro.promise().ps = &ps;
                coro.resume();
                lock.lock();
            }
            EndPaint(dlg_hwnd, &ps);
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
        
        void await_suspend(coroutine_t h)
        {
            std::unique_lock lock{ foreground_mtx };
            timer_ready.push(std::move(h));
        }

        void await_resume() {};
    };

    return awaitable{dlg_hwnd};
}

auto switch_paint()
{
    struct awaitable {
        HWND hwnd;
        PAINTSTRUCT **ps;
        bool await_ready() { return false; }
        
        void await_suspend(coroutine_t h)
        {
            std::unique_lock lock{ foreground_mtx };
            ps = &h.promise().ps;
            paint_ready.push(std::move(h));
        }

        PAINTSTRUCT* await_resume() { return *ps; };
    };

    return awaitable{dlg_hwnd};
}

auto switch_foreground()
{
    struct awaitable {
        HWND hwnd;
        bool await_ready() { return false; }
        
        void await_suspend(coroutine_t h)
        {
            std::unique_lock lock{ foreground_mtx };
            foreground_ready.push(std::move(h));
            PostMessage(dlg_hwnd, WM_USER, 0, 0);
        }

        void await_resume() {};
    };

    return awaitable{dlg_hwnd};
}

coroutine_t query()
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

coroutine_t counter()
{
    auto btn{ GetDlgItem(dlg_hwnd, IDC_BUTTON2) };
    auto lbl{ GetDlgItem(dlg_hwnd, IDC_STATIC2) };

    SetWindowText(lbl, L"");
    EnableWindow(btn, 0);
    for (int i = 0; i < 10; ++i)
    {
        co_await switch_background();
        SleepEx(200, true);

        co_await switch_foreground();
        SetWindowText(lbl, std::to_wstring(i).c_str());
    }
    EnableWindow(btn, true);
}

coroutine_t blink()
{
    std::array<COLORREF, 3> colors{ { RGB(255, 0, 0), RGB(0, 255, 0), RGB(0, 0, 255) } };
    for (int i{}; ; i = (i + 1) % colors.size())
    {
        RECT rect{ .left = 250, .top = 10, .right = 300, .bottom = 60 };
        co_await switch_timer();
        InvalidateRect(dlg_hwnd, &rect, 0);

        auto ps{ co_await switch_paint() };
        SetDCBrushColor(ps->hdc, colors[i]);
        SelectObject(ps->hdc, ::GetStockObject(DC_BRUSH));
        Rectangle(ps->hdc, rect.left, rect.top, rect.right, rect.bottom);
    }
}

int main()
{
    DialogBox(nullptr, MAKEINTRESOURCE(IDD_DIALOG1), nullptr, dialog_proc);
}
