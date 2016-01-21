//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "stdafx.h"

#include <memory>

#include "core/AtomLockGuids.h"

namespace
{

template <typename HandleType, typename DeleterType, DeleterType deleter, HandleType InvalidValue = 0>
struct ScopedGuard
{
    ScopedGuard() = delete;
    ScopedGuard(const ScopedGuard&) = delete;
    void operator=(const ScopedGuard&) = delete;

    ScopedGuard(HandleType handle) : m_handle(handle)
    {
    }

    ~ScopedGuard()
    {
        if (m_handle != InvalidValue)
            deleter(m_handle);
    }

    explicit operator bool() const { return m_handle != InvalidValue; }

    HandleType Get() const { return m_handle; }

private:
    HandleType const m_handle = InvalidValue;
};

template <typename CallType, CallType callee>
struct ScopedCall
{
    ScopedCall(const ScopedCall&) = delete;
    void operator=(const ScopedCall&) = delete;

    ScopedCall() {}

    ~ScopedCall()
    {
        callee();
    }
};

void JsSetCurrentContextToNull()
{
    ChakraRTInterface::JsSetCurrentContext(nullptr);
}

template <typename Handle>
bool WriteFile(const Handle& handle, LPCVOID buffer, DWORD length)
{
    // Ingnore this out value for now.
    DWORD written;
    return !!::WriteFile(handle.Get(), buffer, length, &written, nullptr);
}

template <typename Handle>
bool WriteFile(const Handle& handle, const char* outputStr)
{
    return WriteFile(handle, outputStr, (DWORD)strlen(outputStr));
}

using ScopedAtom = const ScopedGuard<ATOM, decltype(&::DeleteAtom), &::DeleteAtom>;
using ScopedChakraDll = const ScopedGuard<HINSTANCE, decltype(&ChakraRTInterface::UnloadChakraDll), &ChakraRTInterface::UnloadChakraDll, nullptr>;
using ScopedHandle = const ScopedGuard<HANDLE, decltype(&::CloseHandle), &::CloseHandle>;
using ScopedFileHandle = const ScopedGuard<HANDLE, decltype(&::CloseHandle), &::CloseHandle, INVALID_HANDLE_VALUE>;

using ScopedRuntime = const ScopedGuard<
    JsRuntimeHandle,
    decltype(&ChakraRTInterface::JsDisposeRuntime),
    &ChakraRTInterface::JsDisposeRuntime,
    JS_INVALID_RUNTIME_HANDLE>;

using ScopedRestoreContext = const ScopedGuard<
    JsContextRef,
    decltype(&ChakraRTInterface::JsDisposeRuntime),
    &ChakraRTInterface::JsSetCurrentContext,
    JS_INVALID_REFERENCE>;

using ScopedClearContext = const ScopedCall<decltype(&JsSetCurrentContextToNull), &JsSetCurrentContextToNull>;
using ScopedFlushAll = const ScopedCall<decltype(&_flushall), &_flushall>;

}  // namespace

unsigned int MessageBase::s_messageCount = 0;

LPCWSTR hostName = L"ch.exe";

extern "C"
HRESULT __stdcall OnChakraCoreLoadedEntry(TestHooks& testHooks)
{
    return ChakraRTInterface::OnChakraCoreLoaded(testHooks);
}

JsRuntimeAttributes jsrtAttributes = JsRuntimeAttributeAllowScriptInterrupt;
LPCWSTR JsErrorCodeToString(JsErrorCode jsErrorCode)
{
    switch (jsErrorCode)
    {
    case JsNoError:
        return L"JsNoError";
        break;

    case JsErrorInvalidArgument:
        return L"JsErrorInvalidArgument";
        break;

    case JsErrorNullArgument:
        return L"JsErrorNullArgument";
        break;

    case JsErrorNoCurrentContext:
        return L"JsErrorNoCurrentContext";
        break;

    case JsErrorInExceptionState:
        return L"JsErrorInExceptionState";
        break;

    case JsErrorNotImplemented:
        return L"JsErrorNotImplemented";
        break;

    case JsErrorWrongThread:
        return L"JsErrorWrongThread";
        break;

    case JsErrorRuntimeInUse:
        return L"JsErrorRuntimeInUse";
        break;

    case JsErrorBadSerializedScript:
        return L"JsErrorBadSerializedScript";
        break;

    case JsErrorInDisabledState:
        return L"JsErrorInDisabledState";
        break;

    case JsErrorCannotDisableExecution:
        return L"JsErrorCannotDisableExecution";
        break;

    case JsErrorHeapEnumInProgress:
        return L"JsErrorHeapEnumInProgress";
        break;

    case JsErrorOutOfMemory:
        return L"JsErrorOutOfMemory";
        break;

    case JsErrorScriptException:
        return L"JsErrorScriptException";
        break;

    case JsErrorScriptCompile:
        return L"JsErrorScriptCompile";
        break;

    case JsErrorScriptTerminated:
        return L"JsErrorScriptTerminated";
        break;

    case JsErrorFatal:
        return L"JsErrorFatal";
        break;

    default:
        return L"<unknown>";
        break;
    }
}

#define IfJsErrorFailLog(expr) do { JsErrorCode jsErrorCode = expr; if ((jsErrorCode) != JsNoError) { fwprintf(stderr, L"ERROR: " TEXT(#expr) L" failed. JsErrorCode=0x%x (%s)\n", jsErrorCode, JsErrorCodeToString(jsErrorCode)); fflush(stderr); goto Error; } } while (0)
#define IfJsErrorLogReturn(expr) do { JsErrorCode jsErrorCode = expr; if ((jsErrorCode) != JsNoError) { fwprintf(stderr, L"ERROR: " TEXT(#expr) L" failed. JsErrorCode=0x%x (%s)\n", jsErrorCode, JsErrorCodeToString(jsErrorCode)); fflush(stderr); return hr; } } while (0)

int HostExceptionFilter(int exceptionCode, _EXCEPTION_POINTERS *ep)
{
    ChakraRTInterface::NotifyUnhandledException(ep);

    bool crashOnException = false;
    ChakraRTInterface::GetCrashOnExceptionFlag(&crashOnException);

    if (exceptionCode == EXCEPTION_BREAKPOINT || (crashOnException && exceptionCode != 0xE06D7363))
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    fwprintf(stderr, L"FATAL ERROR: %ls failed due to exception code %x\n", hostName, exceptionCode);
    fflush(stderr);

    return EXCEPTION_EXECUTE_HANDLER;
}

void __stdcall PrintUsageFormat()
{
    wprintf(L"\nUsage: ch.exe [flaglist] filename\n");
}

void __stdcall PrintUsage()
{
#ifndef DEBUG
    wprintf(L"\nUsage: ch.exe filename"
            L"\n[flaglist] is not supported for Release mode\n");
#else
    PrintUsageFormat();
    wprintf(L"Try 'ch.exe -?' for help\n");
#endif
}

// On success the param byteCodeBuffer will be allocated in the function.
// The caller of this function should de-allocate the memory.
HRESULT GetSerializedBuffer(LPCOLESTR fileContents, __out BYTE **byteCodeBuffer, __out DWORD *byteCodeBufferSize)
{
    HRESULT hr = S_OK;
    *byteCodeBuffer = nullptr;
    *byteCodeBufferSize = 0;
    BYTE *bcBuffer = nullptr;

    DWORD bcBufferSize = 0;
    IfJsErrorFailLog(ChakraRTInterface::JsSerializeScript(fileContents, bcBuffer, &bcBufferSize));
    // Above call will return the size of the buffer only, once succeed we need to allocate memory of that much and call it again.
    if (bcBufferSize == 0)
    {
        AssertMsg(false, "bufferSize should not be zero");
        IfFailGo(E_FAIL);
    }
    bcBuffer = new BYTE[bcBufferSize];
    DWORD newBcBufferSize = bcBufferSize;
    IfJsErrorFailLog(ChakraRTInterface::JsSerializeScript(fileContents, bcBuffer, &newBcBufferSize));
    Assert(bcBufferSize == newBcBufferSize);

Error:
    if (hr != S_OK)
    {
        // In the failure release the buffer
        if (bcBuffer != nullptr)
        {
            delete[] bcBuffer;
        }
    }
    else
    {
        *byteCodeBuffer = bcBuffer;
        *byteCodeBufferSize = bcBufferSize;
    }

    return hr;
}

HRESULT CreateLibraryByteCodeHeader(LPCOLESTR fileContents, BYTE * contentsRaw, DWORD lengthBytes, LPCWSTR bcFullPath, LPCWSTR libraryNameWide)
{
    HRESULT hr = S_OK;
    BYTE *bcBuffer = nullptr;
    DWORD bcBufferSize = 0;
    IfFailedReturn(GetSerializedBuffer(fileContents, &bcBuffer, &bcBufferSize));
    const std::unique_ptr<BYTE []> bufferGuard(bcBuffer);

    ScopedFileHandle bcFileHandle = CreateFile(bcFullPath, GENERIC_WRITE, FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!bcFileHandle)
    {
        return E_FAIL;
    }

    // For validating the header file against the library file
    auto outputStr =
        "//-------------------------------------------------------------------------------------------------------\r\n"
        "// Copyright (C) Microsoft. All rights reserved.\r\n"
        "// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.\r\n"
        "//-------------------------------------------------------------------------------------------------------\r\n"
        "#if 0\r\n";
    IfFalseReturn(WriteFile(bcFileHandle, outputStr));
    IfFalseReturn(WriteFile(bcFileHandle, contentsRaw, lengthBytes));
    if (lengthBytes < 2 || contentsRaw[lengthBytes - 2] != '\r' || contentsRaw[lengthBytes - 1] != '\n')
    {
        outputStr = "\r\n#endif\r\n";
    }
    else
    {
        outputStr = "#endif\r\n";
    }
    IfFalseReturn(WriteFile(bcFileHandle, outputStr));

    // Write out the bytecode
    IfFalseReturn(WriteFile(bcFileHandle, "namespace Js\r\n{\r\n    const char Library_Bytecode_"));
    size_t convertedChars;
    char libraryNameNarrow[MAX_PATH + 1];
    IfFalseReturn((wcstombs_s(&convertedChars, libraryNameNarrow, libraryNameWide, _TRUNCATE) == 0));
    IfFalseReturn(WriteFile(bcFileHandle, libraryNameNarrow));
    IfFalseReturn(WriteFile(bcFileHandle, "[] = {\r\n/* 00000000 */"));

    for (unsigned int i = 0; i < bcBufferSize; i++)
    {
        char scratch[6];
        auto scratchLen = sizeof(scratch);
        int num = _snprintf_s(scratch, scratchLen, " 0x%02X", bcBuffer[i]);
        Assert(num == 5);
        IfFalseReturn(WriteFile(bcFileHandle, scratch, (DWORD)(scratchLen - 1)));

        // Add a comma and a space if this is not the last item
        if (i < bcBufferSize - 1)
        {
            IfFalseReturn(WriteFile(bcFileHandle, ",", 1));
        }

        // Add a line break every 16 scratches, primarily so the compiler doesn't complain about the string being too long.
        // Also, won't add for the last scratch
        if (i % 16 == 15 && i < bcBufferSize - 1)
        {
            char offset[17];
            _snprintf_s(offset, sizeof(offset), "\r\n/* %08X */", i + 1);  // close quote, new line, offset and open quote
            IfFalseReturn(WriteFile(bcFileHandle, offset));
        }
    }
    IfFalseReturn(WriteFile(bcFileHandle, "};\r\n\r\n"));
    IfFalseReturn(WriteFile(bcFileHandle, "}\r\n"));

    return hr;
}

static void CALLBACK PromiseContinuationCallback(JsValueRef task, void *callbackState)
{
    Assert(task != JS_INVALID_REFERENCE);
    Assert(callbackState != JS_INVALID_REFERENCE);
    MessageQueue * messageQueue = (MessageQueue *)callbackState;

    WScriptJsrt::CallbackMessage *msg = new WScriptJsrt::CallbackMessage(0, task);
    messageQueue->Push(msg);
}

HRESULT RunScript(LPCWSTR fileName, LPCWSTR fileContents, BYTE *bcBuffer, wchar_t *fullPath)
{
    HRESULT hr = S_OK;
    const std::unique_ptr<MessageQueue> messageQueue(new MessageQueue());
    WScriptJsrt::AddMessageQueue(messageQueue.get());

    IfJsErrorFailLog(ChakraRTInterface::JsSetPromiseContinuationCallback(PromiseContinuationCallback, (void*)messageQueue.get()));

    Assert(fileContents != nullptr || bcBuffer != nullptr);
    JsErrorCode runScript;
    if (bcBuffer != nullptr)
    {
        runScript = ChakraRTInterface::JsRunSerializedScript(fileContents, bcBuffer, WScriptJsrt::GetNextSourceContext(), fullPath, nullptr /*result*/);
    }
    else
    {
        runScript = ChakraRTInterface::JsRunScript(fileContents, WScriptJsrt::GetNextSourceContext(), fullPath, nullptr /*result*/);
    }

    if (runScript != JsNoError)
    {
        WScriptJsrt::PrintException(fileName, runScript);
    }
    else
    {
        // Repeatedly flush the message queue until it's empty. It is necessary to loop on this
        // because setTimeout can add scripts to execute.
        do
        {
            IfFailGo(messageQueue->ProcessAll(fileName));
        } while (!messageQueue->IsEmpty());
    }
Error:
    return hr;
}

HRESULT CreateAndRunSerializedScript(LPCWSTR fileName, LPCWSTR fileContents, wchar_t *fullPath)
{
    HRESULT hr = S_OK;
    JsRuntimeHandle runtime = JS_INVALID_RUNTIME_HANDLE;
    JsContextRef context = JS_INVALID_REFERENCE, current = JS_INVALID_REFERENCE;
    BYTE *bcBuffer = nullptr;
    DWORD bcBufferSize = 0;
    IfFailedReturn(GetSerializedBuffer(fileContents, &bcBuffer, &bcBufferSize));
    const std::unique_ptr<BYTE []> bufferGuard(bcBuffer);

    // Bytecode buffer is created in one runtime and will be executed on different runtime.

    IfJsErrorLogReturn(ChakraRTInterface::JsCreateRuntime(jsrtAttributes, nullptr, &runtime));
    ScopedRuntime runtimeGuard(runtime);

    IfJsErrorLogReturn(ChakraRTInterface::JsCreateContext(runtime, &context));
    IfJsErrorLogReturn(ChakraRTInterface::JsGetCurrentContext(&current));
    IfJsErrorLogReturn(ChakraRTInterface::JsSetCurrentContext(context));
    ScopedRestoreContext currentContextGuard(current);

    // Initialized the WScript object on the new context
    if (!WScriptJsrt::Initialize())
    {
        return E_FAIL;
    }

    IfFailedReturn(RunScript(fileName, fileContents, bcBuffer, fullPath));

    return hr;
}

HRESULT ExecuteTest(LPCWSTR fileName)
{
    HRESULT hr = S_OK;
    LPCWSTR fileContents = nullptr;
    JsRuntimeHandle runtime = JS_INVALID_RUNTIME_HANDLE;
    bool isUtf8 = false;
    LPCOLESTR contentsRaw = nullptr;
    UINT lengthBytes = 0;
    hr = Helpers::LoadScriptFromFile(fileName, fileContents, &isUtf8, &contentsRaw, &lengthBytes);
    contentsRaw; lengthBytes; // Unused for now.
    ScopedFlushAll flushGuard;

    IfFailedReturn(hr);
    if (HostConfigFlags::flags.GenerateLibraryByteCodeHeaderIsEnabled)
    {
        jsrtAttributes = (JsRuntimeAttributes)(jsrtAttributes | JsRuntimeAttributeSerializeLibraryByteCode);
    }
    IfJsErrorLogReturn(ChakraRTInterface::JsCreateRuntime(jsrtAttributes, nullptr, &runtime));
    ScopedRuntime runtimeGuard(runtime);

    JsContextRef context = JS_INVALID_REFERENCE;
    IfJsErrorLogReturn(ChakraRTInterface::JsCreateContext(runtime, &context));
    IfJsErrorLogReturn(ChakraRTInterface::JsSetCurrentContext(context));
    ScopedClearContext contextGuard;

    if (!WScriptJsrt::Initialize())
    {
        return E_FAIL;
    }

    wchar_t fullPath[_MAX_PATH];

    if (_wfullpath(fullPath, fileName, _MAX_PATH) == nullptr)
    {
        return E_FAIL;
    }

    // canonicalize that path name to lower case for the profile storage
    size_t len = wcslen(fullPath);
    for (size_t i = 0; i < len; i++)
    {
        fullPath[i] = towlower(fullPath[i]);
    }

    if (HostConfigFlags::flags.GenerateLibraryByteCodeHeaderIsEnabled)
    {
        if (!isUtf8)
        {
            fwprintf(stderr, L"FATAL ERROR: GenerateLibraryByteCodeHeader flag can only be used on UTF8 file, exiting\n");
            return E_FAIL;
        }

        if (HostConfigFlags::flags.GenerateLibraryByteCodeHeader == nullptr || *HostConfigFlags::flags.GenerateLibraryByteCodeHeader == L'\0')
        {
            fwprintf(stderr, L"FATAL ERROR: -GenerateLibraryByteCodeHeader must provide the file name, i.e., -GenerateLibraryByteCodeHeader:<bytecode file name>, exiting\n");
            return E_FAIL;
        }

        WCHAR libraryName[_MAX_PATH];
        WCHAR ext[_MAX_EXT];
        _wsplitpath_s(fullPath, NULL, 0, NULL, 0, libraryName, _countof(libraryName), ext, _countof(ext));

        IfFailedReturn(CreateLibraryByteCodeHeader(fileContents, (BYTE*)contentsRaw, lengthBytes, HostConfigFlags::flags.GenerateLibraryByteCodeHeader, libraryName));
    }
    else if (HostConfigFlags::flags.SerializedIsEnabled)
    {
        if (!isUtf8)
        {
            fwprintf(stderr, L"FATAL ERROR: Serialized flag can only be used on UTF8 file, exiting\n");
            return E_FAIL;
        }

        CreateAndRunSerializedScript(fileName, fileContents, fullPath);
    }
    else
    {
        IfFailedReturn(RunScript(fileName, fileContents, nullptr, fullPath));
    }

    return hr;
}

HRESULT ExecuteTestWithMemoryCheck(BSTR fileName)
{
    HRESULT hr = E_FAIL;
#ifdef CHECK_MEMORY_LEAK
    // Always check memory leak, unless user specified the flag already
    if (!ChakraRTInterface::IsEnabledCheckMemoryFlag())
    {
        ChakraRTInterface::SetCheckMemoryLeakFlag(true);
    }

    // Disable the output in case an unhandled exception happens
    // We will re-enable it if there is no unhandled exceptions
    ChakraRTInterface::SetEnableCheckMemoryLeakOutput(false);
#endif

    __try
    {
        hr = ExecuteTest(fileName);
    }
    __except (HostExceptionFilter(GetExceptionCode(), GetExceptionInformation()))
    {
        _flushall();

        // Exception happened, so we probably didn't clean up properly,
        // Don't exit normally, just terminate
        TerminateProcess(::GetCurrentProcess(), GetExceptionCode());
    }

    _flushall();
#ifdef CHECK_MEMORY_LEAK
    ChakraRTInterface::SetEnableCheckMemoryLeakOutput(true);
#endif
    return hr;
}

unsigned int WINAPI StaticThreadProc(void *lpParam)
{
    ChakraRTInterface::ArgInfo* argInfo = static_cast<ChakraRTInterface::ArgInfo* >(lpParam);
    _endthreadex(ExecuteTestWithMemoryCheck(*(argInfo->filename)));
    return 0;
}

int _cdecl wmain(int argc, __in_ecount(argc) LPWSTR argv[])
{
    if (argc < 2)
    {
        PrintUsage();
        return EXIT_FAILURE;
    }

    HostConfigFlags::pfnPrintUsage = PrintUsageFormat;

    ScopedAtom atomGuard(::AddAtom(szChakraCoreLock));
    AssertMsg(atomGuard, "failed to lock chakracore.dll");

    HostConfigFlags::HandleArgsFlag(argc, argv);

    CComBSTR fileName;

    ChakraRTInterface::ArgInfo argInfo = { argc, argv, PrintUsage, &fileName.m_str };
    ScopedChakraDll chakraLibrary = ChakraRTInterface::LoadChakraDll(argInfo);
    if (!chakraLibrary)
        return 0;

    if (fileName.m_str == nullptr)
    {
        fileName = CComBSTR(argv[1]);
    }

    ScopedHandle threadHandle = reinterpret_cast<HANDLE>(_beginthreadex(0, 0, &StaticThreadProc, &argInfo, STACK_SIZE_PARAM_IS_A_RESERVATION, 0));
    if (!threadHandle)
    {
        fwprintf(stderr, L"FATAL ERROR: failed to create worker thread error code %d, exiting\n", errno);
        AssertMsg(false, "failed to create worker thread");
        return 0;
    }

    const DWORD waitResult = WaitForSingleObject(threadHandle.Get(), INFINITE);
    Assert(waitResult == WAIT_OBJECT_0);

    return 0;
}
