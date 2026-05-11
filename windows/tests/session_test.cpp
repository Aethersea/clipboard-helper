#include "session.h"
#include "test_lite.h"

#include <string>

using leviathan::clipboard_helper::DefaultPipeName;
using leviathan::clipboard_helper::GetCurrentSessionId;

TEST(Session, DefaultPipeNameFormatForZero) {
    EXPECT_EQ(DefaultPipeName(0),
              std::wstring(L"\\\\.\\pipe\\leviathan-clipboard-0"));
}

TEST(Session, DefaultPipeNameFormatForLargeSessionId) {
    EXPECT_EQ(DefaultPipeName(4294967290u),
              std::wstring(L"\\\\.\\pipe\\leviathan-clipboard-4294967290"));
}

TEST(Session, DefaultPipeNameIsDeterministicPerSessionId) {
    EXPECT_EQ(DefaultPipeName(1), DefaultPipeName(1));
    EXPECT_NE(DefaultPipeName(1), DefaultPipeName(2));
}

TEST(Session, GetCurrentSessionIdReturnsAValidSession) {
    // We can't pin the exact session id (depends on the runner), but any
    // logon session is a small non-negative DWORD; sanity-check that the
    // call doesn't crash and that it returns something the per-session
    // pipe name formatter then accepts.
    const auto sid = GetCurrentSessionId();
    const auto name = DefaultPipeName(sid);
    EXPECT_GT(name.size(), static_cast<std::size_t>(0));
    // Pipe name must keep the magic Win32 prefix.
    EXPECT_EQ(name.rfind(L"\\\\.\\pipe\\leviathan-clipboard-",
                         static_cast<std::size_t>(0)),
              static_cast<std::size_t>(0));
}
