/*
 * MIT License
 *
 * Copyright (c) 2017 Serge Zaitsev
 * Copyright (c) 2022 Steffen André Langnes
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef WEBVIEW_DETAIL_ENGINE_BASE_HH
#define WEBVIEW_DETAIL_ENGINE_BASE_HH

#if defined(__cplusplus) && !defined(WEBVIEW_HEADER)

#include "../errors.hh"
#include "../types.h"
#include "../types.hh"
#include "json.hh"
#include "user_script.hh"

#include <atomic>
#include <functional>
#include <list>
#include <map>
#include <string>

namespace webview {
namespace detail {

class engine_base {
public:
  engine_base(bool owns_window) : m_owns_window{owns_window} {}

  virtual ~engine_base() = default;

  noresult navigate(const std::string &url) {
    if (url.empty()) {
      return navigate_impl("about:blank");
    }
    return navigate_impl(url);
  }

  using binding_t = std::function<void(std::string, std::string, void *)>;
  class binding_ctx_t {
  public:
    binding_ctx_t(binding_t callback, void *arg)
        : m_callback(callback), m_arg(arg) {}
    void call(std::string id, std::string args) const {
      if (m_callback) {
        m_callback(id, args, m_arg);
      }
    }

  private:
    // This function is called upon execution of the bound JS function
    binding_t m_callback;
    // This user-supplied argument is passed to the callback
    void *m_arg;
  };

  using sync_binding_t = std::function<std::string(std::string)>;

  // Synchronous bind
  noresult bind(const std::string &name, sync_binding_t fn) {
    auto wrapper = [this, fn](const std::string &id, const std::string &req,
                              void * /*arg*/) { resolve(id, 0, fn(req)); };
    return bind(name, wrapper, nullptr);
  }

  // Asynchronous bind
  noresult bind(const std::string &name, binding_t fn, void *arg) {
    // NOLINTNEXTLINE(readability-container-contains): contains() requires C++20
    if (bindings.count(name) > 0) {
      return error_info{WEBVIEW_ERROR_DUPLICATE};
    }
    bindings.emplace(name, binding_ctx_t(fn, arg));
    replace_bind_script();
    // Notify that a binding was created if the init script has already
    // set things up.
    eval("if (window.__webview__) {\n\
window.__webview__.onBind(" +
         json_escape(name) + ")\n\
}");
    return {};
  }

  noresult unbind(const std::string &name) {
    auto found = bindings.find(name);
    if (found == bindings.end()) {
      return error_info{WEBVIEW_ERROR_NOT_FOUND};
    }
    bindings.erase(found);
    replace_bind_script();
    // Notify that a binding was created if the init script has already
    // set things up.
    eval("if (window.__webview__) {\n\
window.__webview__.onUnbind(" +
         json_escape(name) + ")\n\
}");
    return {};
  }

  noresult resolve(const std::string &id, int status,
                   const std::string &result) {
    // NOLINTNEXTLINE(modernize-avoid-bind): Lambda with move requires C++14
    return dispatch(std::bind(
        [id, status, this](std::string escaped_result) {
          std::string js = "window.__webview__.onReply(" + json_escape(id) +
                           ", " + std::to_string(status) + ", " +
                           escaped_result + ")";
          eval(js);
        },
        result.empty() ? "undefined" : json_escape(result)));
  }

  result<void *> window() { return window_impl(); }
  result<void *> widget() { return widget_impl(); }
  result<void *> browser_controller() { return browser_controller_impl(); }
  noresult run() { return run_impl(); }
  result<int> pump_msgloop(int block) { return pump_msgloop_impl(block); }
  noresult terminate() { return terminate_impl(); }
  noresult dispatch(std::function<void()> f) { return dispatch_impl(f); }
  noresult set_title(const std::string &title) { return set_title_impl(title); }

  noresult set_size(int width, int height, webview_hint_t hints) {
    auto res = set_size_impl(width, height, hints);
    m_is_size_set = true;
    return res;
  }

  noresult set_html(const std::string &html) { return set_html_impl(html); }

  noresult init(const std::string &js) {
    add_user_script(js);
    return {};
  }

  noresult eval(const std::string &js) { return eval_impl(js); }

protected:
  virtual noresult navigate_impl(const std::string &url) = 0;
  virtual result<void *> window_impl() = 0;
  virtual result<void *> widget_impl() = 0;
  virtual result<void *> browser_controller_impl() = 0;
  virtual noresult run_impl() = 0;
  virtual result<int> pump_msgloop_impl(int block) = 0;
  virtual noresult terminate_impl() = 0;
  virtual noresult dispatch_impl(std::function<void()> f) = 0;
  virtual noresult set_title_impl(const std::string &title) = 0;
  virtual noresult set_size_impl(int width, int height,
                                 webview_hint_t hints) = 0;
  virtual noresult set_html_impl(const std::string &html) = 0;
  virtual noresult eval_impl(const std::string &js) = 0;

  virtual user_script *add_user_script(const std::string &js) {
    return std::addressof(*m_user_scripts.emplace(m_user_scripts.end(),
                                                  add_user_script_impl(js)));
  }

  virtual user_script add_user_script_impl(const std::string &js) = 0;

  virtual void
  remove_all_user_scripts_impl(const std::list<user_script> &scripts) = 0;

  virtual bool are_user_scripts_equal_impl(const user_script &first,
                                           const user_script &second) = 0;

  virtual user_script *replace_user_script(const user_script &old_script,
                                           const std::string &new_script_code) {
    remove_all_user_scripts_impl(m_user_scripts);
    user_script *old_script_ptr{};
    for (auto &script : m_user_scripts) {
      auto is_old_script = are_user_scripts_equal_impl(script, old_script);
      script = add_user_script_impl(is_old_script ? new_script_code
                                                  : script.get_code());
      if (is_old_script) {
        old_script_ptr = std::addressof(script);
      }
    }
    return old_script_ptr;
  }

  void replace_bind_script() {
    if (m_bind_script) {
      m_bind_script = replace_user_script(*m_bind_script, create_bind_script());
    } else {
      m_bind_script = add_user_script(create_bind_script());
    }
  }

  void add_init_script(const std::string &post_fn) {
    add_user_script(create_init_script(post_fn));
    m_is_init_script_added = true;
  }

  std::string create_init_script(const std::string &post_fn) {
    auto js = std::string{} + "(function() {\n\
  'use strict';\n\
  const pending = new Map();\n\
  const state = {\n\
    decodeError: (err) => err,\n\
    randomUUID: window.crypto?.randomUUID?.bind(window.crypto),\n\
    withResolvers: Promise?.withResolvers?.bind(Promise),\n\
  };\n\
  if (typeof state.randomUUID !== 'function') {\n\
    state.randomUUID = () => {\n\
      const bytes = new Uint8Array(16);\n\
      window.crypto.getRandomValues(bytes);\n\
      return Array.prototype.slice.call(bytes).map(function(n) {\n\
        return n.toString(16).padStart(2, '0');\n\
      }).join('');\n\
    };\n\
  }\n\
  if (typeof state.withResolvers !== 'function') {\n\
    state.withResolvers = function() {\n\
      let resolve, reject;\n\
      const promise = new Promise(function(_resolve, _reject) {\n\
        resolve = _resolve;\n\
        reject = _reject;\n\
      });\n\
      return { promise, resolve, reject };\n\
    };\n\
  }\n\
  function defer() {\n\
    try {\n\
      const id = state.randomUUID();\n\
      const deferred = state.withResolvers();\n\
      const { promise } = deferred;\n\
      pending.set(id, deferred);\n\
      promise.then(() => pending.delete(id), () => pending.delete(id));\n\
      return Object.assign(deferred, { id });\n\
    } catch (cause) {\n\
      if (cause instanceof Error) return cause;\n\
      return new Error(\"Failed to create deferred promise\", { cause });\n\
    }\n\
  }\n\
  function dispatch(detail) {\n\
    window.dispatchEvent(new CustomEvent('webview:error', { detail }));\n\
  }\n\
  function fail(detail) {\n\
    if (!(detail instanceof Error)) {\n\
      detail = new Error(\"Unknown error\", { cause: detail });\n\
    }\n\
    setTimeout(dispatch, 0, detail);\n\
    return detail;\n\
  }\n\
  function encode(value) {\n\
    try {\n\
      return JSON.stringify(value);\n\
    } catch (cause) {\n\
      throw new Error(\"Webview bridge encode failed\", { cause });\n\
    }\n\
  }\n\
  const api = Object.freeze({\n\
    setDecodeError(fn) {\n\
      if (typeof fn !== 'function') {\n\
        throw new TypeError('decodeError must be a function');\n\
      }\n\
      state.decodeError = fn;\n\
    },\n\
  });\n\
  const bridge = Object.freeze({\n\
    get api() {\n\
      return api;\n\
    },\n\
    post(message) {\n\
      try {\n\
        return (" +
              post_fn + ")(message);\n\
      } catch (cause) {\n\
        throw new Error(\"Webview bridge post failed\", { cause });\n\
      }\n\
    },\n\
    call(method, ...params) {\n\
      const deferred = defer();\n\
      if (deferred instanceof Error) return Promise.reject(fail(deferred));\n\
      try {\n\
        bridge.post(encode({ id: deferred.id, method, params }));\n\
      } catch (cause) {\n\
        deferred.reject(fail(cause));\n\
      }\n\
      return deferred.promise;\n\
    },\n\
    onReply(id, status, result) {\n\
      const deferred = pending.get(id);\n\
      if (!deferred) {\n\
        return;\n\
      }\n\
      try {\n\
        if (result !== undefined) {\n\
          try {\n\
            result = JSON.parse(result);\n\
          } catch (cause) {\n\
            throw new Error(\"Failed to parse results\", {\n\
              cause: { error: cause, suppressed: result },\n\
            });\n\
          }\n\
        }\n\
        if (status === 0) {\n\
          deferred.resolve(result);\n\
        } else {\n\
          try {\n\
            deferred.reject(state.decodeError(result));\n\
          } catch (cause) {\n\
            throw new Error(\"Failed to decode error\", {\n\
              cause: { error: cause, suppressed: result },\n\
            });\n\
          }\n\
        }\n\
      } catch (cause) {\n\
        deferred.reject(fail(cause));\n\
      }\n\
    },\n\
    onBind(name) {\n\
      try {\n\
        if (Object.prototype.hasOwnProperty.call(window, name)) {\n\
          throw new Error('Property \"' + name + '\" already exists');\n\
        }\n\
        window[name] = (...args) => bridge.call(name, ...args);\n\
      } catch (cause) {\n\
        throw fail(cause);\n\
      }\n\
    },\n\
    onUnbind(name) {\n\
      try {\n\
        if (!Object.prototype.hasOwnProperty.call(window, name)) {\n\
          throw new Error('Property \"' + name + '\" does not exist');\n\
        }\n\
        delete window[name];\n\
      } catch (cause) {\n\
        throw fail(cause);\n\
      }\n\
    },\n\
  });\n\
  window.__webview__ = bridge;\n\
})()";
    return js;
  }

  std::string create_bind_script() {
    std::string js_names = "[";
    bool first = true;
    for (const auto &binding : bindings) {
      if (first) {
        first = false;
      } else {
        js_names += ",";
      }
      js_names += json_escape(binding.first);
    }
    js_names += "]";

    auto js = std::string{} + "(function() {\n\
  'use strict';\n\
  var methods = " +
              js_names + ";\n\
  methods.forEach(function(name) {\n\
    try {\n\
      window.__webview__.onBind(name);\n\
    } catch (error) {\n\
      setTimeout(function() {\n\
        throw error;\n\
      }, 0);\n\
    }\n\
  });\n\
})()";
    return js;
  }

  virtual void on_message(const std::string &msg) {
    auto id = json_parse(msg, "id", 0);
    auto name = json_parse(msg, "method", 0);
    auto args = json_parse(msg, "params", 0);
    auto found = bindings.find(name);
    if (found == bindings.end()) {
      return;
    }
    const auto &context = found->second;
    dispatch([=] { context.call(id, args); });
  }

  virtual void on_window_created() { inc_window_count(); }

  virtual void on_window_destroyed(bool skip_termination = false) {
    if (dec_window_count() <= 0) {
      if (!skip_termination) {
        terminate();
      }
    }
  }

  // Runs the event loop until the currently queued events have been processed.
  void deplete_run_loop_event_queue() {
    bool done{};
    dispatch([&] { done = true; });
    run_event_loop_while([&] { return !done; });
  }

  // Runs the event loop while the passed-in function returns true.
  virtual void run_event_loop_while(std::function<bool()> fn) = 0;

  void dispatch_size_default() {
    if (!owns_window() || !m_is_init_script_added) {
      return;
    };
    dispatch([this]() {
      if (!m_is_size_set) {
        set_size(m_initial_width, m_initial_height, WEBVIEW_HINT_NONE);
      }
    });
  }

  void set_default_size_guard(bool guarded) { m_is_size_set = guarded; }

  bool owns_window() const { return m_owns_window; }

private:
  static std::atomic_uint &window_ref_count() {
    static std::atomic_uint ref_count{0};
    return ref_count;
  }

  static unsigned int inc_window_count() { return ++window_ref_count(); }

  static unsigned int dec_window_count() {
    auto &count = window_ref_count();
    if (count > 0) {
      return --count;
    }
    return 0;
  }

  std::map<std::string, binding_ctx_t> bindings;
  user_script *m_bind_script{};
  std::list<user_script> m_user_scripts;

  bool m_is_init_script_added{};
  bool m_is_size_set{};
  bool m_owns_window{};
  static const int m_initial_width = 640;
  static const int m_initial_height = 480;
};

} // namespace detail
} // namespace webview

#endif // defined(__cplusplus) && !defined(WEBVIEW_HEADER)
#endif // WEBVIEW_DETAIL_ENGINE_BASE_HH
