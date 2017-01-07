// Copyright Antony Polukhin, 2016.
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_STACKTRACE_DETAIL_BACKEND_POSIX_HPP
#define BOOST_STACKTRACE_DETAIL_BACKEND_POSIX_HPP

#include <boost/config.hpp>
#ifdef BOOST_HAS_PRAGMA_ONCE
#   pragma once
#endif

#include <boost/core/demangle.hpp>
#include <boost/stacktrace/detail/to_hex_array.hpp>
#include <boost/lexical_cast.hpp>

#include <unwind.h>

#ifdef BOOST_STACKTRACE_USE_BACKTRACE
#include <backtrace.h>
#endif

#include <dlfcn.h>
#include <execinfo.h>
#include <cstdio>

#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>


namespace boost { namespace stacktrace { namespace detail {

class addr2line_pipe {
    FILE* p;
    pid_t pid;

public:
    explicit addr2line_pipe(const char *flag, const char* exec_path, const char* addr) BOOST_NOEXCEPT
        : p(0)
        , pid(0)
    {
        int pdes[2];
        char prog_name[] = "addr2line";
        char* argp[] = {
            prog_name,
            const_cast<char*>(flag),
            const_cast<char*>(exec_path),
            const_cast<char*>(addr),
            0
        };

        if (pipe(pdes) < 0) {
            return;
        }

        pid = fork();
        switch (pid) {
        case -1:
            // failed
            close(pdes[0]);
            close(pdes[1]);
            return;

        case 0:
            // we are the child
            close(STDERR_FILENO);
            close(pdes[0]);
            if (pdes[1] != STDOUT_FILENO) {
                dup2(pdes[1], STDOUT_FILENO);
            }
            execvp(prog_name, argp);
            _exit(127);
        }

        p = fdopen(pdes[0], "r");
        close(pdes[1]);
    }

    operator FILE*() const BOOST_NOEXCEPT {
        return p;
    }

    ~addr2line_pipe() BOOST_NOEXCEPT {
        if (p) {
            fclose(p);
            int pstat = 0;
            kill(pid, SIGKILL);
            waitpid(pid, &pstat, 0);
        }
    }
};

inline std::string addr2line(const char* flag, const void* addr) {
    std::string res;

    Dl_info dli;
    if (!!dladdr(addr, &dli) && dli.dli_fname) {
        res = dli.dli_fname;
    } else {
        res.resize(16);
        int rlin_size = readlink("/proc/self/exe", &res[0], res.size() - 1);
        while (rlin_size == static_cast<int>(res.size() - 1)) {
            res.resize(res.size() * 4);
            rlin_size = readlink("/proc/self/exe", &res[0], res.size() - 1);
        }
        if (rlin_size == -1) {
            res.clear();
            return res;
        }
        res.resize(rlin_size);
    }

    addr2line_pipe p(flag, res.c_str(), to_hex_array(addr).data());
    res.clear();

    if (!p) {
        return res;
    }

    char data[32];
    while (!std::feof(p)) {
        if (std::fgets(data, sizeof(data), p)) {
            res += data;
        } else {
            break;
        }
    }

    // Trimming
    while (!res.empty() && (res[res.size() - 1] == '\n' || res[res.size() - 1] == '\r')) {
        res.erase(res.size() - 1);
    }

    return res;
}

#ifdef BOOST_STACKTRACE_USE_BACKTRACE
struct pc_data {
    unsigned is_function : 1;
    unsigned is_filename : 1;
    
    std::string function;
    std::string filename;
    std::size_t line;
};

static int libbacktrace_full_callback(void *data, uintptr_t pc, const char *filename, int lineno, const char *function) {
    pc_data& d = *static_cast<pc_data*>(data);
    if (d.is_filename && filename) {
        d.filename = filename;
    }
    if (d.is_function && function) {
        d.function = function;
    }
    d.line = lineno;
    return 0;
}
#endif



inline std::string try_demangle(const char* mangled) {
    std::string res;

    boost::core::scoped_demangled_name demangled(mangled);
    if (demangled.get()) {
        res = demangled.get();
    } else {
        res = mangled;
    }

    return res;
}

struct unwind_state {
    void** current;
    void** end;
};

inline _Unwind_Reason_Code unwind_callback(struct _Unwind_Context* context, void* arg) {
    unwind_state* state = static_cast<unwind_state*>(arg);
    *state->current = reinterpret_cast<void*>(
        _Unwind_GetIP(context)
    );

    ++state->current;
    if (!*(state->current - 1) || state->current == state->end) {
        return _URC_END_OF_STACK;
    }
    return _URC_NO_REASON;
}

std::size_t backend::collect(void** memory, std::size_t size) BOOST_NOEXCEPT {
    std::size_t frames_count = 0;
    if (!size) {
        return frames_count;
    }

    unwind_state state = { memory, memory + size };
    _Unwind_Backtrace(&unwind_callback, &state);
    frames_count = state.current - memory;

    if (memory[frames_count - 1] == 0) {
        -- frames_count;
    }

    return frames_count;
}

std::string backend::to_string(const void* addr) {
#ifdef BOOST_STACKTRACE_USE_BACKTRACE
        std::string res;
        ::backtrace_state* state = backtrace_create_state(
            0, 0, 0, 0
        );

        boost::stacktrace::detail::pc_data data;
        data.is_function = 1;
        data.is_filename = 1;
        backtrace_pcinfo(state, reinterpret_cast<uintptr_t>(addr), boost::stacktrace::detail::libbacktrace_full_callback, 0, &data);
        res.swap(data.function);
        if (res.empty()) {
            res = to_hex_array(addr).data();
        }
        
        if (!data.filename.empty() && data.line) {
            res += " at ";
            res += data.filename;
            res += ':';
            res += boost::lexical_cast<boost::array<char, 40> >(data.line).data();
        }else {
            Dl_info dli;
            if (!!dladdr(addr, &dli) && dli.dli_sname) {
                res += " in ";
                res += dli.dli_fname;
            }
        }
        
        return res;
#else
    std::string res = boost::stacktrace::frame(addr).name();
    if (res.empty()) {
        res = to_hex_array(addr).data();
    }
    
    std::string source_line = boost::stacktrace::detail::addr2line("-Cpe", addr);
    if (!source_line.empty() && source_line[0] != '?') {
        res += " at ";
        res += source_line;
    } else {
        Dl_info dli;
        if (!!dladdr(addr, &dli) && dli.dli_sname) {
            res += " in ";
            res += dli.dli_fname;
        }
    }
    
    return res;
    //return addr2line("-Cfipe", addr); // Does not seem to work in all cases
#endif
}

std::string backend::to_string(const frame* frames, std::size_t size) {
    std::string res;
    res.reserve(64 * size);
    for (std::size_t i = 0; i < size; ++i) {
        if (i < 10) {
            res += ' ';
        }
        res += boost::lexical_cast<boost::array<char, 40> >(i).data();
        res += '#';
        res += ' ';
        res += to_string(frames[i].address());
        res += '\n';
    }

    return res;
}




} // namespace detail

std::string frame::name() const {
    std::string res;

    Dl_info dli;
    const bool dl_ok = !!dladdr(addr_, &dli);
    if (dl_ok && dli.dli_sname) {
        res = boost::stacktrace::detail::try_demangle(dli.dli_sname);
    } else {
#ifdef BOOST_STACKTRACE_USE_BACKTRACE
        ::backtrace_state* state = backtrace_create_state(
            (dl_ok ? dli.dli_fname : 0), 0, 0, 0
        );

        boost::stacktrace::detail::pc_data data;
        data.is_function = 1;
        data.is_filename = 0;
        backtrace_pcinfo(state, reinterpret_cast<uintptr_t>(addr_), boost::stacktrace::detail::libbacktrace_full_callback, 0, &data);
        if (!data.function.empty()) {
            return boost::stacktrace::detail::try_demangle(data.function.c_str());
        }
#endif
        
        res = boost::stacktrace::detail::addr2line("-fe", addr_);
        res = res.substr(0, res.find_last_of('\n'));
        res = boost::stacktrace::detail::try_demangle(res.c_str());
    }

    if (res == "??") {
        res.clear();
    }

    return res;
}

std::string frame::source_file() const {
#ifdef BOOST_STACKTRACE_USE_BACKTRACE
        ::backtrace_state* state = backtrace_create_state(
            0, 0, 0, 0
        );

        boost::stacktrace::detail::pc_data data;
        data.is_function = 0;
        data.is_filename = 1;
        backtrace_pcinfo(state, reinterpret_cast<uintptr_t>(addr_), boost::stacktrace::detail::libbacktrace_full_callback, 0, &data);
        if (!data.filename.empty()) {
            return data.filename;
        }
#endif

    std::string res = boost::stacktrace::detail::addr2line("-e", addr_);
    res = res.substr(0, res.find_last_of(':'));
    if (res == "??") {
        res.clear();
    }
    return res;
}

std::size_t frame::source_line() const {
#ifdef BOOST_STACKTRACE_USE_BACKTRACE
        ::backtrace_state* state = backtrace_create_state(
            0, 0, 0, 0
        );

        boost::stacktrace::detail::pc_data data;
        data.is_function = 0;
        data.is_filename = 0;
        backtrace_pcinfo(state, reinterpret_cast<uintptr_t>(addr_), boost::stacktrace::detail::libbacktrace_full_callback, 0, &data);
        if (data.line) {
            return data.line;
        }
#endif


    std::string res = boost::stacktrace::detail::addr2line("-e", addr_);
    const std::size_t last = res.find_last_of(':');
    if (last == std::string::npos) {
        return 0;
    }
    res = res.substr(last + 1);

    std::size_t line_num = 0;
    if (!boost::conversion::try_lexical_convert(res, line_num)) {
        return 0;
    }

    return line_num;
}


}} // namespace boost::stacktrace

#endif // BOOST_STACKTRACE_DETAIL_BACKEND_POSIX_HPP
