#include "jit_code_buffer.h"
#include "align.h"
#include "assert.h"
#include "common/log.h"
#include "cpu_detect.h"
#include <algorithm>
Log_SetChannel(JitCodeBuffer);

#if defined(WIN32)
#include "windows_headers.h"
#else
#include <errno.h>
#include <sys/mman.h>
#endif

JitCodeBuffer::JitCodeBuffer() = default;

JitCodeBuffer::JitCodeBuffer(u32 size, u32 far_code_size)
{
  if (!Allocate(size, far_code_size))
    Panic("Failed to allocate code space");
}

JitCodeBuffer::JitCodeBuffer(void* buffer, u32 size, u32 far_code_size, u32 guard_pages)
{
  if (!Initialize(buffer, size, far_code_size))
    Panic("Failed to initialize code space");
}

JitCodeBuffer::~JitCodeBuffer()
{
  Destroy();
}

bool JitCodeBuffer::Allocate(u32 size /* = 64 * 1024 * 1024 */, u32 far_code_size /* = 0 */)
{
  Destroy();

  m_total_size = size + far_code_size;

#if defined(WIN32)
  m_code_ptr = static_cast<u8*>(VirtualAlloc(nullptr, m_total_size, MEM_COMMIT, PAGE_EXECUTE_READWRITE));
  if (!m_code_ptr)
  {
    Log_ErrorPrintf("VirtualAlloc(RWX, %u) for internal buffer failed: %u", m_total_size, GetLastError());
    return false;
  }
#elif defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__) || defined(__HAIKU__)
  m_code_ptr = static_cast<u8*>(
    mmap(nullptr, m_total_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (!m_code_ptr)
  {
    Log_ErrorPrintf("mmap(RWX, %u) for internal buffer failed: %d", m_total_size, errno);
    return false;
  }
#else
  return false;
#endif

  m_free_code_ptr = m_code_ptr;
  m_code_size = size;
  m_code_used = 0;

  m_far_code_ptr = static_cast<u8*>(m_code_ptr) + size;
  m_free_far_code_ptr = m_far_code_ptr;
  m_far_code_size = far_code_size;
  m_far_code_used = 0;

  m_old_protection = 0;
  m_owns_buffer = true;
  return true;
}

bool JitCodeBuffer::Initialize(void* buffer, u32 size, u32 far_code_size /* = 0 */, u32 guard_size /* = 0 */)
{
  Destroy();

  if ((far_code_size > 0 && guard_size >= far_code_size) || (far_code_size + (guard_size * 2)) > size)
    return false;

#if defined(WIN32)
  DWORD old_protect = 0;
  if (!VirtualProtect(buffer, size, PAGE_EXECUTE_READWRITE, &old_protect))
  {
    Log_ErrorPrintf("VirtualProtect(RWX) for external buffer failed: %u", GetLastError());
    return false;
  }

  if (guard_size > 0)
  {
    DWORD old_guard_protect = 0;
    u8* guard_at_end = (static_cast<u8*>(buffer) + size) - guard_size;
    if (!VirtualProtect(buffer, guard_size, PAGE_NOACCESS, &old_guard_protect) ||
        !VirtualProtect(guard_at_end, guard_size, PAGE_NOACCESS, &old_guard_protect))
    {
      Log_ErrorPrintf("VirtualProtect(NOACCESS) for guard page failed: %u", GetLastError());
      return false;
    }
  }

  m_code_ptr = static_cast<u8*>(buffer);
  m_old_protection = static_cast<u32>(old_protect);
#elif defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__) || defined(__HAIKU__)
  if (mprotect(buffer, size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
  {
    Log_ErrorPrintf("mprotect(RWX) for external buffer failed: %d", errno);
    return false;
  }

  if (guard_size > 0)
  {
    u8* guard_at_end = (static_cast<u8*>(buffer) + size) - guard_size;
    if (mprotect(buffer, guard_size, PROT_NONE) != 0 || mprotect(guard_at_end, guard_size, PROT_NONE) != 0)
    {
      Log_ErrorPrintf("mprotect(NONE) for guard page failed: %d", errno);
      return false;
    }
  }

  // reasonable default?
  m_code_ptr = static_cast<u8*>(buffer);
  m_old_protection = PROT_READ | PROT_WRITE;
#else
  m_code_ptr = nullptr;
#endif

  if (!m_code_ptr)
    return false;

  m_total_size = size;
  m_free_code_ptr = m_code_ptr + guard_size;
  m_code_size = size - far_code_size - (guard_size * 2);
  m_code_used = 0;

  m_far_code_ptr = static_cast<u8*>(m_code_ptr) + m_code_size;
  m_free_far_code_ptr = m_far_code_ptr;
  m_far_code_size = far_code_size - guard_size;
  m_far_code_used = 0;

  m_guard_size = guard_size;
  m_owns_buffer = false;
  return true;
}

void JitCodeBuffer::Destroy()
{
  if (m_owns_buffer)
  {
#if defined(WIN32)
    VirtualFree(m_code_ptr, 0, MEM_RELEASE);
#elif defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__) || defined(__HAIKU__)
    munmap(m_code_ptr, m_total_size);
#endif
  }
  else if (m_code_ptr)
  {
#if defined(WIN32)
    DWORD old_protect = 0;
    VirtualProtect(m_code_ptr, m_total_size, m_old_protection, &old_protect);
#else
    mprotect(m_code_ptr, m_total_size, m_old_protection);
#endif
  }
}

void JitCodeBuffer::CommitCode(u32 length)
{
  if (length == 0)
    return;

#if defined(CPU_AARCH32) || defined(CPU_AARCH64)
  // ARM instruction and data caches are not coherent, we need to flush after every block.
  FlushInstructionCache(m_free_code_ptr, length);
#endif

  Assert(length <= (m_code_size - m_code_used));
  m_free_code_ptr += length;
  m_code_used += length;
}

void JitCodeBuffer::CommitFarCode(u32 length)
{
  if (length == 0)
    return;

#if defined(CPU_AARCH32) || defined(CPU_AARCH64)
  // ARM instruction and data caches are not coherent, we need to flush after every block.
  FlushInstructionCache(m_free_far_code_ptr, length);
#endif

  Assert(length <= (m_far_code_size - m_far_code_used));
  m_free_far_code_ptr += length;
  m_far_code_used += length;
}

void JitCodeBuffer::Reset()
{
  m_free_code_ptr = m_code_ptr + m_guard_size;
  m_code_used = 0;
  std::memset(m_free_code_ptr, 0, m_code_size);
  FlushInstructionCache(m_free_code_ptr, m_code_size);

  if (m_far_code_size > 0)
  {
    m_free_far_code_ptr = m_far_code_ptr;
    m_far_code_used = 0;
    std::memset(m_free_far_code_ptr, 0, m_far_code_size);
    FlushInstructionCache(m_free_far_code_ptr, m_far_code_size);
  }
}

void JitCodeBuffer::Align(u32 alignment, u8 padding_value)
{
  DebugAssert(Common::IsPow2(alignment));
  const u32 num_padding_bytes =
    std::min(static_cast<u32>(Common::AlignUpPow2(reinterpret_cast<uintptr_t>(m_free_code_ptr), alignment) -
                              reinterpret_cast<uintptr_t>(m_free_code_ptr)),
             GetFreeCodeSpace());
  std::memset(m_free_code_ptr, padding_value, num_padding_bytes);
  m_free_code_ptr += num_padding_bytes;
  m_code_used += num_padding_bytes;
}

void JitCodeBuffer::FlushInstructionCache(void* address, u32 size)
{
#if defined(WIN32)
  ::FlushInstructionCache(GetCurrentProcess(), address, size);
#elif defined(__GNUC__) || defined(__clang__)
  //__builtin___clear_cache(reinterpret_cast<char*>(address), reinterpret_cast<char*>(address) + size);
#else
#error Unknown platform.
#endif
}
