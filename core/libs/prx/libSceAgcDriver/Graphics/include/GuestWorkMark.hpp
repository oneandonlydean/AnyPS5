#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTWORKMARK_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTWORKMARK_HPP

#include <array>
#include <cstddef>

namespace AgcDriver::Graphics::GuestWorkMark {

// The command buffers this thread recorded guest draws into and has not submitted yet (run report first-draw milestone). A mark is
// consumed by the successful submit of that buffer and dropped when the buffer is released or its batch fails, so a handle that is
// freed and handed out again never carries a stale mark. Handles are compared as plain addresses.
struct Marks {
    std::array<const void*, 8> buffers{};
    std::size_t count = 0;
};

inline Marks& Local() {
    thread_local Marks marks;
    return marks;
}

inline void Note(const void* commands) {
    auto& marks = Local();
    for (std::size_t index = 0; index < marks.count; ++index) if (marks.buffers[index] == commands) return;
    if (marks.count == marks.buffers.size()) marks.buffers[0] = marks.buffers[--marks.count];
    marks.buffers[marks.count++] = commands;
}

// True (and the mark is gone) when `commands` was marked.
inline bool Take(const void* commands) {
    auto& marks = Local();
    for (std::size_t index = 0; index < marks.count; ++index) {
        if (marks.buffers[index] != commands) continue;
        marks.buffers[index] = marks.buffers[--marks.count];
        return true;
    }
    return false;
}

// How many buffers carry a mark on this thread (tests).
inline std::size_t Count() {
    return Local().count;
}

inline void Forget(const void* commands) {
    static_cast<void>(Take(commands));
}

}

#endif
