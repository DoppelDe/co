// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#ifndef coroutine_h
#define coroutine_h

// We have two modes of context switches available.  The most
// portable is using setjmp/longjmp with a little assembly
// language to switch stacks for the first call.  There is
// also user contexts which is a System V facility that is
// available on Linux and other operating systems.
//
// TODO: maybe I need to write my own context switching functions
// if the OS providers are going to remove features.  They seem
// to be forcing everything into threads, which is the antithesis
// of coroutines.
#define CTX_SETJMP 1
#define CTX_UCONTEXT 2

// Apple has deprecated user contexts so we can't use them
// on MacOS.  Linux still has them and there's an issue with
// using setjmp/longjmp on Linux when running with LLVM
// TSAN.  It assumes that a longjmp is always to the same
// stack as the setjmp used.  That's kind of the point of
// coroutines.  It's also not possible to suppress the
// longjmp interception in TSAN, so if you want to make
// use of TSAN in something that uses coroutines, you have to
// use user contexts.
#if defined(__APPLE__)
#define CTX_MODE CTX_SETJMP
#include <csetjmp>
#elif defined(__linux__)
// Linux supports user contexts.  Let's use them so that tsan works.
#define CTX_MODE CTX_UCONTEXT
#include <ucontext.h>
#else
// Portable version is setjmp/longjmp
#define CTX_MODE CTX_SETJMP
#include <csetjmp>
#endif

#include <poll.h>

#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <list>
#include <string>
#include <vector>

#include "bitset.h"

namespace co {

class CoroutineScheduler;
class Coroutine;
template <typename T>
class Generator;

using CoroutineFunction = std::function<void(Coroutine *)>;
using CompletionCallback = std::function<void(Coroutine *)>;

template <typename T>
using GeneratorFunction = std::function<void(Generator<T> *)>;

constexpr size_t kCoDefaultStackSize = 32 * 1024;

extern "C" {
// This is needed here because it's a friend with C linkage.
void __co_Invoke(class Coroutine *c);
}

template <typename T>
class Generator;

// This is a Coroutine.  It executes its function (pointer to a function
// or a lambda).
//
// It has its own stack with default size kCoDefaultStackSize.
// By default, the coroutine will be given a unique name and will
// be started automatically.  It can have some user data which is
// not owned by the coroutine.
class Coroutine {
 public:
  Coroutine(CoroutineScheduler &machine, CoroutineFunction function,
            const char *name = nullptr, bool autostart = true,
            size_t stack_size = kCoDefaultStackSize, void *user_data = nullptr);

  ~Coroutine();

  // Start a coroutine running if it is not already running,
  void Start();

  // Yield control to another coroutine.
  void Yield();

  // Call another coroutine and store the result.
  template <typename T>
  T Call(Generator<T> &callee);

  // For all Wait functions, the timeout is optional and if greater than zero
  // specifies a nanosecond timeout.  If the timeout occurs before the fd (or
  // one of the fds) becomes ready, Wait will return -1. If an fd is ready, Wait
  // will return the fd that terminated the wait.

  // Wait for a file descriptor to become ready.  Returns the fd if it
  // was triggered or -1 for timeout.
  int Wait(int fd, short event_mask = POLLIN, uint64_t timeout_ns = 0);

  // Wait for a pollfd.   Returns the fd if it was triggered or -1 for timeout.
  int Wait(struct pollfd &fd, uint64_t timeout_ns = 0);

  // Wait for a set of pollfds.  Each needs to specify an fd and an event.
  // Returns the fd that was triggered, or -1 for a timeout.
  int Wait(const std::vector<struct pollfd> &fds, uint64_t timeout_ns = 0);

  void Exit();

  // Sleeping functions.
  void Nanosleep(uint64_t ns);
  void Millisleep(time_t msecs) {
    Nanosleep(static_cast<uint64_t>(msecs) * 1000000LL);
  }
  void Sleep(time_t secs) {
    Nanosleep(static_cast<uint64_t>(secs) * 1000000000LL);
  }

  // Set and get the name.  You can change the name at any time.  It's
  // only for debug really.
  void SetName(const std::string &name) { name_ = name; }
  const std::string &Name() const { return name_; }

  // Set and get the user data (not owned by the coroutine).  It's up
  // to you what this contains and you are responsible for its
  // management.
  void SetUserData(void *user_data) { user_data_ = user_data; }
  void *UserData() const { return user_data_; }

  // Is the given coroutine alive?
  bool IsAlive() const;

  uint64_t LastTick() const { return last_tick_; }
  CoroutineScheduler &Scheduler() const { return scheduler_; }

  void Show() const;

  // Each coroutine has a unique id.
  uint32_t Id() const { return id_; }

  void SeToStringCallback(std::function<std::string()> cb) {
    to_string_callback_ = std::move(cb);
  }

  // Make a string describing information about this coroutine.  By default
  // this will be the same as that printed by Show().
  std::string ToString() const;

 private:
  enum class State {
    kCoNew,
    kCoReady,
    kCoRunning,
    kCoYielded,
    kCoWaiting,
    kCoDead,
  };
  friend class CoroutineScheduler;
  template <typename T>
  friend class Generator;

  friend void __co_Invoke(Coroutine *c);
  void InvokeFunction();
  int EndOfWait(int timer_fd);
  int AddTimeout(uint64_t timeout_ns);
  State GetState() const { return state_; }
  void AddPollFds(std::vector<struct pollfd> &pollfds,
                  std::vector<Coroutine *> &covec);
  void Resume(int value);
  void TriggerEvent();
  void ClearEvent();
  void CallNonTemplate(Coroutine &c);
  void YieldNonTemplate();

  std::string MakeDefaultString() const;

  CoroutineScheduler &scheduler_;
  uint32_t id_;                 // Coroutine ID.
  CoroutineFunction function_;  // Coroutine body.
  std::string name_;            // Optional name.
  State state_;
  void *stack_;                      // Stack, allocated from malloc.
  void *yielded_address_ = nullptr;  // Address at which we've yielded.
  size_t stack_size_;
#if CTX_MODE == CTX_SETJMP
  jmp_buf resume_;  // Program environemnt for resuming.
  jmp_buf exit_;    // Program environemt to exit.
#else
  ucontext_t resume_;
  ucontext_t exit_;
#endif
  int wait_result_;
  bool first_resume_ = true;

  struct pollfd event_fd_;               // Pollfd for event.
  std::vector<struct pollfd> wait_fds_;  // Pollfds for waiting for an fd.
  Coroutine *caller_ = nullptr;          // If being called, who is calling us.
  void *user_data_;                      // User data, not owned by this.
  uint64_t last_tick_ = 0;               // Tick count of last resume.

  // Function used to create a string for this coroutine.
  std::function<std::string()> to_string_callback_;
};

// A Generator is a coroutine that generates values.  The magic lamda line
// noise is because you can't cast an std::function<void(B*)> to an
// std::function<void(A*)> even though B is derived from A.
//
// A generator doesn't start automatically.  It's started on the
// first call.
template <typename T>
class Generator : public Coroutine {
 public:
  Generator(CoroutineScheduler &machine, GeneratorFunction<T> function,
            const char *name = nullptr, size_t stack_size = kCoDefaultStackSize,
            void *user_data = nullptr)
      : Coroutine(machine,
                  [this](Coroutine *c) {
                    gen_function_(reinterpret_cast<Generator<T> *>(c));
                  },
                  name, /*autostart=*/false, stack_size, user_data),
        gen_function_(function) {}

  // Yield control and store value.
  void YieldValue(const T &value);

 private:
  friend class Coroutine;
  GeneratorFunction<T> gen_function_;
  T *result_ = nullptr;  // Where to put result in YieldValue.
};

struct PollState {
  std::vector<struct pollfd> pollfds;
  std::vector<Coroutine *> coroutines;
};

class CoroutineScheduler {
 public:
  CoroutineScheduler();
  ~CoroutineScheduler();

  // Run the scheduler until all coroutines have terminated or
  // told to stop.
  void Run();

  // Stop the scheduler.  Running coroutines will not be terminated.
  void Stop();

  void AddCoroutine(Coroutine *c);
  void RemoveCoroutine(Coroutine *c);
  void StartCoroutine(Coroutine *c);

  // When you don't want to use the Run function, these
  // functions allow you to incorporate the multiplexed
  // IO into your own poll loop.
  void GetPollState(PollState *poll_state);
  void ProcessPoll(PollState *poll_state);

  // Print the state of all the coroutines to stderr.
  void Show();

  // Call the given function when a coroutine exits.
  // You can use this to delete the coroutine.
  void SetCompletionCallback(CompletionCallback callback) {
    completion_callback_ = callback;
  }

  // Get a vector containing all the strings generated by the
  // coroutines.
  std::vector<std::string> AllCoroutineStrings() const;

 private:
  friend class Coroutine;
  template <typename T>
  friend class Generator;
  struct ChosenCoroutine {
    ChosenCoroutine() = default;
    ChosenCoroutine(Coroutine *c, int f) : co(c), fd(f) {}
    Coroutine *co = nullptr;
    int fd = 0x12345678;
  };

  void BuildPollFds(PollState *poll_state);
  ChosenCoroutine ChooseRunnable(PollState *poll_state, int num_ready);

  ChosenCoroutine GetRunnableCoroutine(PollState *poll_state, int num_ready);
  uint32_t AllocateId();
  uint64_t TickCount() const { return tick_count_; }
  bool IdExists(uint32_t id) const { return coroutine_ids_.Contains(id); }
#if CTX_MODE == CTX_SETJMP
  jmp_buf &YieldBuf() { return yield_; }
#else
  ucontext_t *YieldCtx() { return &yield_; }
#endif

  std::list<Coroutine *> coroutines_;
  BitSet coroutine_ids_;
  uint32_t last_freed_coroutine_id_ = -1U;
#if CTX_MODE == CTX_SETJMP
  jmp_buf yield_;
#else
  ucontext_t yield_;
#endif
  bool running_ = false;
  PollState poll_state_;
  struct pollfd interrupt_fd_;
  uint64_t tick_count_ = 0;
  CompletionCallback completion_callback_;
};

template <typename T>
inline void Generator<T>::YieldValue(const T &value) {
  // Copy value.
  if (result_ != nullptr) {
    *result_ = value;
  }
  YieldNonTemplate();
}

template <typename T>
inline T Coroutine::Call(Generator<T> &callee) {
  T result;
  // Tell the callee that it's being called and where to store the value.
  callee.caller_ = this;
  callee.result_ = &result;
  CallNonTemplate(callee);
  // Call done.  No result now.
  callee.result_ = nullptr;
  return result;
}

}  // namespace co
#endif /* coroutine_h */
