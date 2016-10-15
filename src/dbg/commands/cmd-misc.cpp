#include "cmd-misc.h"
#include "exprfunc.h"
#include "variable.h"
#include "value.h"
#include "debugger.h"
#include "threading.h"
#include "thread.h"
#include "assemble.h"
#include "memory.h"
#include "plugin_loader.h"
#include "jit.h"
#include "mnemonichelp.h"
#include "commandline.h"

bool cbInstrChd(int argc, char* argv[])
{
    if(IsArgumentsLessThan(argc, 2))
        return false;
    if(!DirExists(argv[1]))
    {
        dputs(QT_TRANSLATE_NOOP("DBG", "directory doesn't exist"));
        return false;
    }
    SetCurrentDirectoryW(StringUtils::Utf8ToUtf16(argv[1]).c_str());
    dputs(QT_TRANSLATE_NOOP("DBG", "current directory changed!"));
    return true;
}

bool cbInstrZzz(int argc, char* argv[])
{
    duint value = 100;
    if(argc > 1)
        if(!valfromstring(argv[1], &value, false))
            return false;
    auto ms = DWORD(value);
    if(ms == INFINITE)
        ms = 100;
    Sleep(ms);
    return true;
}

bool cbDebugHide(int argc, char* argv[])
{
    if(HideDebugger(fdProcessInfo->hProcess, UE_HIDE_PEBONLY))
        dputs(QT_TRANSLATE_NOOP("DBG", "Debugger hidden"));
    else
        dputs(QT_TRANSLATE_NOOP("DBG", "Something went wrong"));
    return true;
}

static duint LoadLibThreadID;
static duint DLLNameMem;
static duint ASMAddr;
static TITAN_ENGINE_CONTEXT_t backupctx = { 0 };

static void cbDebugLoadLibBPX()
{
    HANDLE LoadLibThread = ThreadGetHandle((DWORD)LoadLibThreadID);
#ifdef _WIN64
    duint LibAddr = GetContextDataEx(LoadLibThread, UE_RAX);
#else
    duint LibAddr = GetContextDataEx(LoadLibThread, UE_EAX);
#endif //_WIN64
    varset("$result", LibAddr, false);
    backupctx.eflags &= ~0x100;
    SetFullContextDataEx(LoadLibThread, &backupctx);
    MemFreeRemote(DLLNameMem);
    MemFreeRemote(ASMAddr);
    ThreadResumeAll();
    //update GUI
    DebugUpdateGuiSetStateAsync(GetContextDataEx(hActiveThread, UE_CIP), true);
    //lock
    lock(WAITID_RUN);
    dbgsetforeground();
    PLUG_CB_PAUSEDEBUG pauseInfo = { nullptr };
    plugincbcall(CB_PAUSEDEBUG, &pauseInfo);
    wait(WAITID_RUN);
}

bool cbDebugLoadLib(int argc, char* argv[])
{
    if(argc < 2)
    {
        dputs(QT_TRANSLATE_NOOP("DBG", "Error: you must specify the name of the DLL to load\n"));
        return false;
    }

    LoadLibThreadID = fdProcessInfo->dwThreadId;
    HANDLE LoadLibThread = ThreadGetHandle((DWORD)LoadLibThreadID);

    DLLNameMem = MemAllocRemote(0, strlen(argv[1]) + 1);
    ASMAddr = MemAllocRemote(0, 0x1000);

    if(!DLLNameMem || !ASMAddr)
    {
        dputs(QT_TRANSLATE_NOOP("DBG", "Error: couldn't allocate memory in debuggee"));
        return false;
    }

    if(!MemWrite(DLLNameMem, argv[1], strlen(argv[1])))
    {
        dputs(QT_TRANSLATE_NOOP("DBG", "Error: couldn't write process memory"));
        return false;
    }

    int size = 0;
    int counter = 0;
    duint LoadLibraryA = 0;
    char command[50] = "";
    char error[MAX_ERROR_SIZE] = "";

    GetFullContextDataEx(LoadLibThread, &backupctx);

    if(!valfromstring("kernel32:LoadLibraryA", &LoadLibraryA, false))
    {
        dputs(QT_TRANSLATE_NOOP("DBG", "Error: couldn't get kernel32:LoadLibraryA"));
        return false;
    }

    // Arch specific asm code
#ifdef _WIN64
    sprintf_s(command, "mov rcx, %p", DLLNameMem);
#else
    sprintf_s(command, "push %p", DLLNameMem);
#endif // _WIN64

    assembleat(ASMAddr, command, &size, error, true);
    counter += size;

#ifdef _WIN64
    sprintf_s(command, "mov rax, %p", LoadLibraryA);
    assembleat(ASMAddr + counter, command, &size, error, true);
    counter += size;
    sprintf_s(command, "call rax");
#else
    sprintf_s(command, "call %p", LoadLibraryA);
#endif // _WIN64

    assembleat(ASMAddr + counter, command, &size, error, true);
    counter += size;

    SetContextDataEx(LoadLibThread, UE_CIP, ASMAddr);
    auto ok = SetBPX(ASMAddr + counter, UE_SINGLESHOOT | UE_BREAKPOINT_TYPE_INT3, (void*)cbDebugLoadLibBPX);

    ThreadSuspendAll();
    ResumeThread(LoadLibThread);

    unlock(WAITID_RUN);

    return ok;
}

bool cbInstrAssemble(int argc, char* argv[])
{
    if(IsArgumentsLessThan(argc, 3))
        return false;
    duint addr = 0;
    if(!valfromstring(argv[1], &addr))
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "invalid expression: \"%s\"!\n"), argv[1]);
        return false;
    }
    if(!DbgMemIsValidReadPtr(addr))
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "invalid address: %p!\n"), addr);
        return false;
    }
    bool fillnop = false;
    if(argc > 3)
        fillnop = true;
    char error[MAX_ERROR_SIZE] = "";
    int size = 0;
    if(!assembleat(addr, argv[2], &size, error, fillnop))
    {
        varset("$result", size, false);
        dprintf(QT_TRANSLATE_NOOP("DBG", "failed to assemble \"%s\" (%s)\n"), argv[2], error);
        return false;
    }
    varset("$result", size, false);
    GuiUpdateAllViews();
    return true;
}

bool cbInstrGpa(int argc, char* argv[])
{
    if(IsArgumentsLessThan(argc, 2))
        return false;
    char newcmd[deflen] = "";
    if(argc >= 3)
        sprintf_s(newcmd, "\"%s\":%s", argv[2], argv[1]);
    else
        sprintf_s(newcmd, "%s", argv[1]);
    duint result = 0;
    if(!valfromstring(newcmd, &result, false))
        return false;
    varset("$RESULT", result, false);
    return true;
}

bool cbDebugSetJIT(int argc, char* argv[])
{
    arch actual_arch = invalid;
    char* jit_debugger_cmd = "";
    Memory<char*> oldjit(MAX_SETTING_SIZE + 1);
    char path[JIT_ENTRY_DEF_SIZE];
    if(!IsProcessElevated())
    {
        dputs(QT_TRANSLATE_NOOP("DBG", "Error run the debugger as Admin to setjit\n"));
        return false;
    }
    if(argc < 2)
    {
        dbggetdefjit(path);

        jit_debugger_cmd = path;
        if(!dbgsetjit(jit_debugger_cmd, notfound, &actual_arch, NULL))
        {
            dprintf(QT_TRANSLATE_NOOP("DBG", "Error setting JIT %s\n"), (actual_arch == x64) ? "x64" : "x32");
            return false;
        }
    }
    else if(argc == 2)
    {
        if(!_strcmpi(argv[1], "old"))
        {
            jit_debugger_cmd = oldjit();
            if(!BridgeSettingGet("JIT", "Old", jit_debugger_cmd))
            {
                dputs(QT_TRANSLATE_NOOP("DBG", "Error there is no old JIT entry stored."));
                return false;
            }

            if(!dbgsetjit(jit_debugger_cmd, notfound, &actual_arch, NULL))
            {
                dprintf(QT_TRANSLATE_NOOP("DBG", "Error setting JIT %s\n"), (actual_arch == x64) ? "x64" : "x32");
                return false;
            }
        }
        else if(!_strcmpi(argv[1], "oldsave"))
        {
            dbggetdefjit(path);
            char get_entry[JIT_ENTRY_MAX_SIZE] = "";
            bool get_last_jit = true;

            if(!dbggetjit(get_entry, notfound, &actual_arch, NULL))
            {
                get_last_jit = false;
            }
            else
                strcpy_s(oldjit(), MAX_SETTING_SIZE, get_entry);

            jit_debugger_cmd = path;
            if(!dbgsetjit(jit_debugger_cmd, notfound, &actual_arch, NULL))
            {
                dprintf(QT_TRANSLATE_NOOP("DBG", "Error setting JIT %s\n"), (actual_arch == x64) ? "x64" : "x32");
                return false;
            }
            if(get_last_jit)
            {
                if(_stricmp(oldjit(), path))
                    BridgeSettingSet("JIT", "Old", oldjit());
            }
        }
        else if(!_strcmpi(argv[1], "restore"))
        {
            jit_debugger_cmd = oldjit();

            if(!BridgeSettingGet("JIT", "Old", jit_debugger_cmd))
            {
                dputs(QT_TRANSLATE_NOOP("DBG", "Error there is no old JIT entry stored."));
                return false;
            }

            if(!dbgsetjit(jit_debugger_cmd, notfound, &actual_arch, NULL))
            {
                dprintf(QT_TRANSLATE_NOOP("DBG", "Error setting JIT %s\n"), (actual_arch == x64) ? "x64" : "x32");
                return false;
            }
            BridgeSettingSet("JIT", 0, 0);
        }
        else
        {
            jit_debugger_cmd = argv[1];
            if(!dbgsetjit(jit_debugger_cmd, notfound, &actual_arch, NULL))
            {
                dprintf(QT_TRANSLATE_NOOP("DBG", "Error setting JIT %s\n"), (actual_arch == x64) ? "x64" : "x32");
                return false;
            }
        }
    }
    else if(argc == 3)
    {
        readwritejitkey_error_t rw_error;

        if(!_strcmpi(argv[1], "old"))
        {
            BridgeSettingSet("JIT", "Old", argv[2]);

            dprintf(QT_TRANSLATE_NOOP("DBG", "New OLD JIT stored: %s\n"), argv[2]);

            return true;
        }

        else if(_strcmpi(argv[1], "x64") == 0)
            actual_arch = x64;
        else if(_strcmpi(argv[1], "x32") == 0)
            actual_arch = x32;
        else
        {
            dputs(QT_TRANSLATE_NOOP("DBG", "Unknown JIT entry type. Use OLD, x64 or x32 as parameter."));
            return false;
        }

        jit_debugger_cmd = argv[2];
        if(!dbgsetjit(jit_debugger_cmd, actual_arch, NULL, &rw_error))
        {
            if(rw_error == ERROR_RW_NOTWOW64)
                dputs(QT_TRANSLATE_NOOP("DBG", "Error using x64 arg. The debugger is not a WOW64 process\n"));
            else
                dprintf(QT_TRANSLATE_NOOP("DBG", "Error setting JIT %s\n"), (actual_arch == x64) ? "x64" : "x32");
            return false;
        }
    }
    else
    {
        dputs(QT_TRANSLATE_NOOP("DBG", "Error unknown parameters. Use old, oldsave, restore, x86 or x64 as parameter."));
        return false;
    }

    dprintf(QT_TRANSLATE_NOOP("DBG", "New JIT %s: %s\n"), (actual_arch == x64) ? "x64" : "x32", jit_debugger_cmd);

    return true;
}

bool cbDebugGetJIT(int argc, char* argv[])
{
    char get_entry[JIT_ENTRY_MAX_SIZE] = "";
    arch actual_arch;

    if(argc < 2)
    {
        if(!dbggetjit(get_entry, notfound, &actual_arch, NULL))
        {
            dprintf(QT_TRANSLATE_NOOP("DBG", "Error getting JIT %s\n"), (actual_arch == x64) ? "x64" : "x32");
            return false;
        }
    }
    else
    {
        readwritejitkey_error_t rw_error;
        Memory<char*> oldjit(MAX_SETTING_SIZE + 1);
        if(_strcmpi(argv[1], "OLD") == 0)
        {
            if(!BridgeSettingGet("JIT", "Old", oldjit()))
            {
                dputs(QT_TRANSLATE_NOOP("DBG", "Error: there is not an OLD JIT entry stored yet."));
                return false;
            }
            else
            {
                dprintf(QT_TRANSLATE_NOOP("DBG", "OLD JIT entry stored: %s\n"), oldjit());
                return true;
            }
        }
        else if(_strcmpi(argv[1], "x64") == 0)
            actual_arch = x64;
        else if(_strcmpi(argv[1], "x32") == 0)
            actual_arch = x32;
        else
        {
            dputs(QT_TRANSLATE_NOOP("DBG", "Unknown JIT entry type. Use OLD, x64 or x32 as parameter."));
            return false;
        }

        if(!dbggetjit(get_entry, actual_arch, NULL, &rw_error))
        {
            if(rw_error == ERROR_RW_NOTWOW64)
                dputs(QT_TRANSLATE_NOOP("DBG", "Error using x64 arg. The debugger is not a WOW64 process\n"));
            else
                dprintf(QT_TRANSLATE_NOOP("DBG", "Error getting JIT %s\n"), argv[1]);
            return false;
        }
    }

    dprintf(QT_TRANSLATE_NOOP("DBG", "JIT %s: %s\n"), (actual_arch == x64) ? "x64" : "x32", get_entry);

    return true;
}

bool cbDebugGetJITAuto(int argc, char* argv[])
{
    bool jit_auto = false;
    arch actual_arch = invalid;

    if(argc == 1)
    {
        if(!dbggetjitauto(&jit_auto, notfound, &actual_arch, NULL))
        {
            dprintf(QT_TRANSLATE_NOOP("DBG", "Error getting JIT auto %s\n"), (actual_arch == x64) ? "x64" : "x32");
            return false;
        }
    }
    else if(argc == 2)
    {
        readwritejitkey_error_t rw_error;
        if(_strcmpi(argv[1], "x64") == 0)
            actual_arch = x64;
        else if(_strcmpi(argv[1], "x32") == 0)
            actual_arch = x32;
        else
        {
            dputs(QT_TRANSLATE_NOOP("DBG", "Unknown JIT auto entry type. Use x64 or x32 as parameter."));
            return false;
        }

        if(!dbggetjitauto(&jit_auto, actual_arch, NULL, &rw_error))
        {
            if(rw_error == ERROR_RW_NOTWOW64)
                dputs(QT_TRANSLATE_NOOP("DBG", "Error using x64 arg the debugger is not a WOW64 process\n"));
            else
                dprintf(QT_TRANSLATE_NOOP("DBG", "Error getting JIT auto %s\n"), argv[1]);
            return false;
        }
    }
    else
    {
        dputs(QT_TRANSLATE_NOOP("DBG", "Unknown JIT auto entry type. Use x64 or x32 as parameter"));
    }

    dprintf(QT_TRANSLATE_NOOP("DBG", "JIT auto %s: %s\n"), (actual_arch == x64) ? "x64" : "x32", jit_auto ? "ON" : "OFF");

    return true;
}

bool cbDebugSetJITAuto(int argc, char* argv[])
{
    arch actual_arch;
    bool set_jit_auto;
    if(!IsProcessElevated())
    {
        dputs(QT_TRANSLATE_NOOP("DBG", "Error run the debugger as Admin to setjitauto\n"));
        return false;
    }
    if(argc < 2)
    {
        dputs(QT_TRANSLATE_NOOP("DBG", "Error setting JIT Auto. Use ON:1 or OFF:0 arg or x64/x32, ON:1 or OFF:0.\n"));
        return false;
    }
    else if(argc == 2)
    {
        if(_strcmpi(argv[1], "1") == 0 || _strcmpi(argv[1], "ON") == 0)
            set_jit_auto = true;
        else if(_strcmpi(argv[1], "0") == 0 || _strcmpi(argv[1], "OFF") == 0)
            set_jit_auto = false;
        else
        {
            dputs(QT_TRANSLATE_NOOP("DBG", "Error unknown parameters. Use ON:1 or OFF:0"));
            return false;
        }

        if(!dbgsetjitauto(set_jit_auto, notfound, &actual_arch, NULL))
        {
            if(actual_arch == x64)
                dputs(QT_TRANSLATE_NOOP("DBG", "Error setting JIT auto x64"));
            else
                dputs(QT_TRANSLATE_NOOP("DBG", "Error setting JIT auto x32"));
            return false;
        }
    }
    else if(argc == 3)
    {
        readwritejitkey_error_t rw_error;
        actual_arch = x64;

        if(_strcmpi(argv[1], "x64") == 0)
            actual_arch = x64;
        else if(_strcmpi(argv[1], "x32") == 0)
            actual_arch = x32;
        else
        {
            dputs(QT_TRANSLATE_NOOP("DBG", "Unknown JIT auto entry type. Use x64 or x32 as parameter"));
            return false;
        }

        if(_strcmpi(argv[2], "1") == 0 || _strcmpi(argv[2], "ON") == 0)
            set_jit_auto = true;
        else if(_strcmpi(argv[2], "0") == 0 || _strcmpi(argv[2], "OFF") == 0)
            set_jit_auto = false;
        else
        {
            dputs(QT_TRANSLATE_NOOP("DBG", "Error unknown parameters. Use x86 or x64 and ON:1 or OFF:0\n"));
            return false;
        }

        if(!dbgsetjitauto(set_jit_auto, actual_arch, NULL, &rw_error))
        {
            if(rw_error == ERROR_RW_NOTWOW64)
                dputs(QT_TRANSLATE_NOOP("DBG", "Error using x64 arg the debugger is not a WOW64 process\n"));
            else
            {
                if(actual_arch == x64)
                    dputs(QT_TRANSLATE_NOOP("DBG", "Error getting JIT auto x64"));
                else
                    dputs(QT_TRANSLATE_NOOP("DBG", "Error getting JIT auto x32"));
            }
            return false;
        }
    }
    else
    {
        dputs(QT_TRANSLATE_NOOP("DBG", "Error unknown parameters use x86 or x64, ON/1 or OFF/0\n"));
        return false;
    }

    dprintf(QT_TRANSLATE_NOOP("DBG", "New JIT auto %s: %s\n"), (actual_arch == x64) ? "x64" : "x32", set_jit_auto ? "ON" : "OFF");
    return true;
}

bool cbDebugGetCmdline(int argc, char* argv[])
{
    char* cmd_line;
    cmdline_error_t cmdline_error = { (cmdline_error_type_t)0, 0 };

    if(!dbggetcmdline(&cmd_line, &cmdline_error))
    {
        showcommandlineerror(&cmdline_error);
        return false;
    }

    dprintf(QT_TRANSLATE_NOOP("DBG", "Command line: %s\n"), cmd_line);

    efree(cmd_line);

    return true;
}

bool cbDebugSetCmdline(int argc, char* argv[])
{
    cmdline_error_t cmdline_error = { (cmdline_error_type_t)0, 0 };

    if(argc != 2)
    {
        dputs(QT_TRANSLATE_NOOP("DBG", "Error: write the arg1 with the new command line of the process debugged"));
        return false;
    }

    if(!dbgsetcmdline(argv[1], &cmdline_error))
    {
        showcommandlineerror(&cmdline_error);
        return false;
    }

    //update the memory map
    MemUpdateMap();
    GuiUpdateMemoryView();

    dprintf(QT_TRANSLATE_NOOP("DBG", "New command line: %s\n"), argv[1]);

    return true;
}

bool cbInstrMnemonichelp(int argc, char* argv[])
{
    if(IsArgumentsLessThan(argc, 2))
        return false;
    auto description = MnemonicHelp::getDescription(argv[1]);
    if(!description.length())
        dputs(QT_TRANSLATE_NOOP("DBG", "no description or empty description"));
    else
    {
        auto padding = "================================================================";
        auto logText = StringUtils::sprintf("%s%s%s\n", padding, description.c_str(), padding);
        GuiAddLogMessage(logText.c_str());
    }
    return true;
}

bool cbInstrMnemonicbrief(int argc, char* argv[])
{
    if(IsArgumentsLessThan(argc, 2))
        return false;
    dputs(MnemonicHelp::getBriefDescription(argv[1]).c_str());
    return true;
}