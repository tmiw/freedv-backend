//==========================================================================
// Name:            windows_interface.cpp
//
// Purpose:         Implements Windowes specific logic for OS functions.
// Created:         December 31, 2024
// Authors:         Mooneer Salem
// 
// License:
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// - Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// - Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
// OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//==========================================================================

#include "os_interface.h"

#include <windows.h>
//#include <wx/wx.h>
//#include <wx/msw/registry.h>

void VerifyMicrophonePermissions(std::promise<bool>& micPromise)
{
#if 0
    bool microphonePermissionsGranted = false;
    
    // General Microphone enable/disable (applies to all users)
    wxRegKey localMachineMicrophoneKey(wxRegKey::HKLM, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone");

    bool localMachineMicrophoneAllowed = false;
    if (localMachineMicrophoneKey.Exists())
    {
        wxString regValue;
        localMachineMicrophoneKey.QueryValue("Value", regValue, true);
        localMachineMicrophoneAllowed = regValue == "Allow";
    }
    
    microphonePermissionsGranted = localMachineMicrophoneAllowed;
    micPromise.set_value(microphonePermissionsGranted);
#else
    // Bypass all microphone checks for now. On some systems, the above registry key isn't correct and I'm
    // not sure why (or what, if any key, should be used instead). This check will either need to be fixed 
    // for a future release or fully eliminated.
    micPromise.set_value(true);
#endif // 0
}

void ResetMainWindowColorSpace()
{
    // empty
}

void StartLowLatencyActivity()
{
    // empty
}

void StopLowLatencyActivity()
{
    // empty
}

std::string GetOperatingSystemString()
{
    return "windows";
}

void SetThreadName(std::string const& name)
{
    // XXX - assumes ASCII. This is probably fine, though, since
    // this is debug code.
    std::string fullName = "FDV ";
    fullName += name;
    std::wstring stemp = std::wstring(fullName.begin(), fullName.end());

    // SetThreadDescription is Windows 10 1607+ only, and isn't even
    // declared by every mingw-w64 header revision (e.g. the one Ubuntu
    // 22.04's gcc-mingw-w64 package ships, versus llvm-mingw's much newer
    // bundled headers) -- resolve it dynamically instead of calling it
    // directly so this builds/runs regardless of toolchain or OS version.
    using SetThreadDescriptionFn = HRESULT (WINAPI*)(HANDLE, PCWSTR);
    static auto pSetThreadDescription = reinterpret_cast<SetThreadDescriptionFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));
    if (pSetThreadDescription != nullptr)
    {
        pSetThreadDescription(GetCurrentThread(), stemp.c_str());
    }
}
