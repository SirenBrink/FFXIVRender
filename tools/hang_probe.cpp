#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <iostream>
#include <string>
int main(int argc,char**argv){
 if(argc!=2)return 2;DWORD pid=std::stoul(argv[1]);
 HANDLE process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,pid);if(!process)return 3;
 SymSetOptions(SYMOPT_DEFERRED_LOADS|SYMOPT_UNDNAME|SYMOPT_FAIL_CRITICAL_ERRORS|SYMOPT_NO_PROMPTS);
 SymInitialize(process,"C:/Games/FF14/XIVRender/build",TRUE);
 HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);THREADENTRY32 entry{};entry.dwSize=sizeof(entry);
 if(Thread32First(snapshot,&entry))do{if(entry.th32OwnerProcessID!=pid)continue;
  HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_SUSPEND_RESUME|THREAD_QUERY_INFORMATION,FALSE,entry.th32ThreadID);if(!thread)continue;
  if(SuspendThread(thread)==DWORD(-1)){CloseHandle(thread);continue;}
  CONTEXT context{};context.ContextFlags=CONTEXT_FULL;
  if(GetThreadContext(thread,&context)){
   std::cout<<"THREAD "<<entry.th32ThreadID<<'\n';STACKFRAME64 frame{};frame.AddrPC={context.Rip,0,AddrModeFlat};frame.AddrFrame={context.Rbp,0,AddrModeFlat};frame.AddrStack={context.Rsp,0,AddrModeFlat};
   for(unsigned n=0;n<32;++n){
    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO)+1024]{};auto*symbol=reinterpret_cast<SYMBOL_INFO*>(storage);symbol->SizeOfStruct=sizeof(SYMBOL_INFO);symbol->MaxNameLen=1023;
    DWORD64 displacement=0;IMAGEHLP_MODULE64 module{};module.SizeOfStruct=sizeof(module);SymGetModuleInfo64(process,frame.AddrPC.Offset,&module);
    std::cout<<"  0x"<<std::hex<<frame.AddrPC.Offset<<' '<<module.ModuleName;
    if(SymFromAddr(process,frame.AddrPC.Offset,&displacement,symbol))std::cout<<'!'<<symbol->Name<<"+0x"<<displacement;else std::cout<<"+0x"<<frame.AddrPC.Offset-module.BaseOfImage;std::cout<<std::dec<<'\n';
    if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64,process,thread,&frame,&context,nullptr,SymFunctionTableAccess64,SymGetModuleBase64,nullptr)||!frame.AddrPC.Offset)break;
   }
  }
  ResumeThread(thread);CloseHandle(thread);
 }while(Thread32Next(snapshot,&entry));
 CloseHandle(snapshot);SymCleanup(process);CloseHandle(process);return 0;
}
