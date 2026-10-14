"""Launch owned QA processes without network capabilities; no host policy changes.

Uses the documented Win32 AppContainer APIs and deletes its unique profile.
Filesystem permissions are granted by the caller only to copied QA payloads.
https://learn.microsoft.com/en-us/windows/win32/secauthz/implementing-an-appcontainer
"""

import ctypes as C
from ctypes import wintypes as W
import os
import subprocess
import time
import uuid

import psutil


class StartupInfo(C.Structure):
    _fields_ = [
        ("cb", W.DWORD),
        ("reserved", W.LPWSTR),
        ("desktop", W.LPWSTR),
        ("title", W.LPWSTR),
        *[
            (name, W.DWORD)
            for name in (
                "x",
                "y",
                "width",
                "height",
                "chars_x",
                "chars_y",
                "fill",
                "flags",
            )
        ],
        ("show", W.WORD),
        ("reserved_size", W.WORD),
        ("reserved_bytes", C.c_void_p),
        ("stdin", W.HANDLE),
        ("stdout", W.HANDLE),
        ("stderr", W.HANDLE),
    ]


class StartupInfoEx(C.Structure):
    _fields_ = [("startup", StartupInfo), ("attributes", C.c_void_p)]


class ProcessInfo(C.Structure):
    _fields_ = [
        ("process", W.HANDLE),
        ("thread", W.HANDLE),
        ("pid", W.DWORD),
        ("tid", W.DWORD),
    ]


class Capabilities(C.Structure):
    _fields_ = [
        ("sid", C.c_void_p),
        ("capabilities", C.c_void_p),
        ("count", W.DWORD),
        ("reserved", W.DWORD),
    ]


def api(library, name, arguments, result=W.BOOL):
    function = getattr(library, name)
    function.argtypes = arguments
    function.restype = result
    return function


class AppContainer:
    def __init__(self):
        if os.name != "nt":
            raise RuntimeError("AppContainer QA requires Windows")
        self.name = "PDFTatsujin.QA." + uuid.uuid4().hex
        self.kernel = C.WinDLL("kernel32", use_last_error=True)
        self.security = C.WinDLL("advapi32", use_last_error=True)
        self.profiles = C.WinDLL("userenv", use_last_error=True)
        self.close = api(self.kernel, "CloseHandle", [W.HANDLE])
        self.sid = C.c_void_p()
        create = api(
            self.profiles,
            "CreateAppContainerProfile",
            [
                W.LPCWSTR,
                W.LPCWSTR,
                W.LPCWSTR,
                C.c_void_p,
                W.DWORD,
                C.POINTER(C.c_void_p),
            ],
            C.c_long,
        )
        status = create(
            self.name,
            self.name,
            "Temporary PDFTatsujin QA profile",
            None,
            0,
            C.byref(self.sid),
        )
        if status != 0:
            raise RuntimeError(
                f"CreateAppContainerProfile failed: 0x{status & 0xffffffff:08x}"
            )
        text = W.LPWSTR()
        convert = api(
            self.security, "ConvertSidToStringSidW", [C.c_void_p, C.POINTER(W.LPWSTR)]
        )
        if not convert(self.sid, C.byref(text)):
            self.remove()
            raise C.WinError(C.get_last_error())
        self.sid_text = text.value
        api(self.kernel, "LocalFree", [C.c_void_p], C.c_void_p)(
            C.cast(text, C.c_void_p)
        )
        self.removed = False

    def token(self, pid):
        open_process = api(
            self.kernel, "OpenProcess", [W.DWORD, W.BOOL, W.DWORD], W.HANDLE
        )
        handle = open_process(0x1000, False, pid)
        if not handle:
            raise C.WinError(C.get_last_error())
        token = W.HANDLE()
        try:
            if not api(
                self.security,
                "OpenProcessToken",
                [W.HANDLE, W.DWORD, C.POINTER(W.HANDLE)],
            )(handle, 8, C.byref(token)):
                raise C.WinError(C.get_last_error())
            query = api(
                self.security,
                "GetTokenInformation",
                [W.HANDLE, W.DWORD, C.c_void_p, W.DWORD, C.POINTER(W.DWORD)],
            )
            container = W.DWORD()
            size = W.DWORD()
            if not query(
                token, 29, C.byref(container), C.sizeof(container), C.byref(size)
            ):
                raise C.WinError(C.get_last_error())
            query(token, 30, None, 0, C.byref(size))
            buffer = C.create_string_buffer(size.value)
            if not query(token, 30, buffer, size.value, C.byref(size)):
                raise C.WinError(C.get_last_error())
            return {
                "is_AppContainer": container.value,
                "capability_count": C.cast(buffer, C.POINTER(W.DWORD)).contents.value,
            }
        finally:
            if token:
                self.close(token)
            self.close(handle)

    def run(self, arguments, environment, cwd, timeout=180, observe=None):
        size = C.c_size_t()
        initialize = api(
            self.kernel,
            "InitializeProcThreadAttributeList",
            [C.c_void_p, W.DWORD, W.DWORD, C.POINTER(C.c_size_t)],
        )
        initialize(None, 1, 0, C.byref(size))
        if not size.value:
            raise C.WinError(C.get_last_error())
        attributes = C.create_string_buffer(size.value)
        if not initialize(attributes, 1, 0, C.byref(size)):
            raise C.WinError(C.get_last_error())
        remove_attributes = api(
            self.kernel, "DeleteProcThreadAttributeList", [C.c_void_p], None
        )
        info = ProcessInfo()
        error_mode = api(self.kernel, "SetErrorMode", [W.DWORD], W.DWORD)
        previous_error_mode = error_mode(0x8003)
        try:
            capabilities = Capabilities(self.sid, None, 0, 0)
            update = api(
                self.kernel,
                "UpdateProcThreadAttribute",
                [
                    C.c_void_p,
                    W.DWORD,
                    C.c_size_t,
                    C.c_void_p,
                    C.c_size_t,
                    C.c_void_p,
                    C.c_void_p,
                ],
            )
            if not update(
                attributes,
                0,
                0x20009,
                C.byref(capabilities),
                C.sizeof(capabilities),
                None,
                None,
            ):
                raise C.WinError(C.get_last_error())
            startup = StartupInfoEx()
            startup.startup.cb = C.sizeof(startup)
            startup.startup.flags = 1
            startup.attributes = C.cast(attributes, C.c_void_p)
            command = C.create_unicode_buffer(
                subprocess.list2cmdline([str(a) for a in arguments])
            )
            block = C.create_unicode_buffer(
                "\0".join(
                    f"{k}={v}"
                    for k, v in sorted(environment.items(), key=lambda p: p[0].upper())
                )
                + "\0\0"
            )
            create = api(
                self.kernel,
                "CreateProcessW",
                [
                    W.LPCWSTR,
                    W.LPWSTR,
                    C.c_void_p,
                    C.c_void_p,
                    W.BOOL,
                    W.DWORD,
                    C.c_void_p,
                    W.LPCWSTR,
                    C.POINTER(StartupInfoEx),
                    C.POINTER(ProcessInfo),
                ],
            )
            if not create(
                str(arguments[0]),
                command,
                None,
                None,
                False,
                0x80000 | 0x400 | 0x8000000,
                block,
                str(cwd),
                C.byref(startup),
                C.byref(info),
            ):
                raise C.WinError(C.get_last_error())
            wait = api(self.kernel, "WaitForSingleObject", [W.HANDLE, W.DWORD], W.DWORD)
            started = time.monotonic()
            while True:
                state = wait(info.process, 20)
                if state == 0:
                    break
                if state != 258:
                    raise C.WinError(C.get_last_error())
                if time.monotonic() - started > timeout:
                    raise RuntimeError("AppContainer QA process timed out")
                if observe:
                    observe(info.pid)
            result = W.DWORD()
            if not api(
                self.kernel, "GetExitCodeProcess", [W.HANDLE, C.POINTER(W.DWORD)]
            )(info.process, C.byref(result)):
                raise C.WinError(C.get_last_error())
            return result.value
        except Exception:
            if (
                info.process
                and api(
                    self.kernel, "WaitForSingleObject", [W.HANDLE, W.DWORD], W.DWORD
                )(info.process, 0)
                == 258
            ):
                for child in psutil.Process(info.pid).children(recursive=True):
                    try:
                        child.kill()
                    except psutil.NoSuchProcess:
                        pass
                api(self.kernel, "TerminateProcess", [W.HANDLE, W.UINT])(
                    info.process, 2
                )
                api(self.kernel, "WaitForSingleObject", [W.HANDLE, W.DWORD], W.DWORD)(
                    info.process, 10000
                )
            raise
        finally:
            error_mode(previous_error_mode)
            if info.thread:
                self.close(info.thread)
            if info.process:
                self.close(info.process)
            remove_attributes(attributes)

    def remove(self):
        delete = api(self.profiles, "DeleteAppContainerProfile", [W.LPCWSTR], C.c_long)
        status = delete(self.name)
        api(self.security, "FreeSid", [C.c_void_p], C.c_void_p)(self.sid)
        self.sid = C.c_void_p()
        self.removed = status == 0
        if status:
            raise RuntimeError(
                f"DeleteAppContainerProfile failed: 0x{status & 0xffffffff:08x}"
            )
