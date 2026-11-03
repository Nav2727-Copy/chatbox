#pragma once
#include "network/tls.h"
#include "ui/curses_ui.h"
#include <curses.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

// Blocking library teardown (notably native DNS) stays owned by the worker.
// Certificate decisions are marshalled to the terminal thread, never drawn by it.
template<class Result, class Work>
std::optional<Result> run_network_task(chatbox::tls::ClientConfig config, Work work)
{
    struct Decision { std::string address, fingerprint, previous; };
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        std::atomic<bool> cancelled{false}, done{false};
        std::optional<Decision> decision;
        std::optional<bool> answer;
        std::optional<Result> result;
        std::string error;
    };
    auto state = std::make_shared<State>();
    auto confirm = config.confirm_certificate;
    config.continue_waiting = [state] { return !state->cancelled; };
    config.confirm_certificate = [state](const std::string& address, const std::string& fingerprint, const std::string& previous) {
        std::unique_lock lock(state->mutex);
        if (state->cancelled) return false;
        state->decision = Decision{address, fingerprint, previous};
        state->changed.wait(lock, [&] { return state->answer.has_value() || state->cancelled; });
        const bool accepted = !state->cancelled && state->answer.value_or(false);
        state->answer.reset(); return accepted;
    };
    std::thread([state, config, work = std::move(work)]() mutable {
        try { state->result = work(config); }
        catch (const std::exception& error) { state->error = error.what(); }
        state->done = true;
    }).detach();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!state->done) {
        std::optional<Decision> decision;
        { std::lock_guard lock(state->mutex); decision = std::move(state->decision); state->decision.reset(); }
        if (decision) {
            const bool answer = confirm && confirm(decision->address, decision->fingerprint, decision->previous);
            { std::lock_guard lock(state->mutex); state->answer = answer; }
            state->changed.notify_all();
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        }
        show_curses_notice("Contacting directory", "Connecting or reading results. Esc cancels; your settings are preserved.", false);
        timeout(50);
        if (getch() == 27 || std::chrono::steady_clock::now() >= deadline) {
            state->cancelled = true; state->changed.notify_all(); timeout(-1); return {};
        }
    }
    timeout(-1);
    if (!state->error.empty()) throw std::runtime_error(state->error);
    return std::move(state->result);
}
