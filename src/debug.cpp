#include <dlfcn.h>
#include <backward.hpp>

#include <iqdb/debug.h>

namespace iqdb {

int debug_level = 0; // Debug

thread_local std::string last_exception_backtrace;

// Deep magic. Override the C++ `throw` statement to save a backtrace when an
// exception is thrown. This is done by overriding `__cxa_throw`, which is an
// internal library function called by the C++ runtime when an exception is thrown.
//
// http://itanium-cxx-abi.github.io/cxx-abi/abi-eh.html#cxx-throw
// https://stackoverflow.com/a/11674810
extern "C" void __cxa_throw(void* exception, std::type_info* type_info, void (*destructor)(void*)) {
  last_exception_backtrace = get_backtrace(2);

  decltype(__cxa_throw)* rethrow = (decltype(__cxa_throw)*)dlsym(RTLD_NEXT, "__cxa_throw");
  rethrow(exception, type_info, destructor);
}

// Generate a backtrace using the Backward library.
// https://github.com/bombela/backward-cpp#stacktrace.
std::string get_backtrace(int skip) {
  backward::StackTrace trace;
  backward::Printer printer;
  std::ostringstream stream;

  trace.load_here(32);
  trace.skip_n_firsts(skip);

  printer.address = true;
  printer.object = true;
  printer.print(trace, stream);

  return stream.str();
}

// Demangle a C++ symbol name.
std::string demangle_name(std::string symbol_name) {
  // https://gcc.gnu.org/onlinedocs/libstdc++/libstdc++-html-USERS-4.3/a01696.html
  char* buffer = abi::__cxa_demangle(symbol_name.c_str(), NULL, NULL, NULL);

  if (buffer) {
    std::string demangled_name(buffer);
    free(buffer);
    return demangled_name;
  } else {
    throw std::runtime_error("__cxa_demangle failed");
  }
}

}
