#include "gmb/process.hpp"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace gmb::io {
namespace {
struct Handle {
    HANDLE h=nullptr;
    ~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}
};
std::wstring wide(const std::string& text) {
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);
    if(n==0)throw std::runtime_error("Invalid UTF-8 process argument.");
    std::wstring value(n,L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),value.data(),n);return value;
}
std::wstring quote(const std::string& value) {
    std::wstring result=L"\"";std::size_t slashes=0;
    for(auto c:wide(value)) {
        if(c==L'\\'){++slashes;continue;}
        result.append(c==L'\"'?slashes*2+1:slashes,L'\\');slashes=0;result+=c;
    }
    result.append(slashes*2,L'\\');return result+L'\"';
}
void drain(HANDLE pipe,std::string& tail,bool& truncated) {
    constexpr std::size_t limit=256*1024;
    // Limit work per iteration so a continuously writing process cannot starve
    // the timeout check or the other stream.
    for(unsigned batch=0;batch<16;++batch) {
        DWORD available=0;
        if(!PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr)) {
            if(GetLastError()==ERROR_BROKEN_PIPE)return;
            throw std::runtime_error("MAX_LOG_READ_ERROR: Cannot inspect process pipe.");
        }
        if(!available)return;
        char buffer[16384];DWORD count=0;
        if(!ReadFile(pipe,buffer,std::min<DWORD>(available,sizeof(buffer)),&count,nullptr))
            throw std::runtime_error("MAX_LOG_READ_ERROR: Cannot read process pipe.");
        tail.append(buffer,count);
        if(tail.size()>limit){tail.erase(0,tail.size()-limit);truncated=true;}
    }
}
}
ProcessResult run_max_process(const std::vector<std::string>& args,
                             const std::filesystem::path& directory,unsigned timeout) {
    if(args.empty()||timeout==0)throw std::runtime_error("Invalid MAX process request.");
    // Closing the last job handle kills all descendants, including when the CLI
    // is cancelled by the GUI or terminated externally.
    Handle job{CreateJobObjectW(nullptr,nullptr)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job.h||!SetInformationJobObject(job.h,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))
        throw std::runtime_error("MAX_PROCESS_ERROR: Cannot create isolated process job.");
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    Handle out_read,out_write,err_read,err_write,input;
    if(!CreatePipe(&out_read.h,&out_write.h,&sa,0)||!CreatePipe(&err_read.h,&err_write.h,&sa,0)||
       !SetHandleInformation(out_read.h,HANDLE_FLAG_INHERIT,0)||!SetHandleInformation(err_read.h,HANDLE_FLAG_INHERIT,0))
        throw std::runtime_error("MAX_PROCESS_ERROR: Cannot create capture pipes.");
    input.h=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr);
    if(input.h==INVALID_HANDLE_VALUE)throw std::runtime_error("MAX_PROCESS_ERROR: Cannot open null input.");
    STARTUPINFOEXW start{};start.StartupInfo.cb=sizeof(start);
    start.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    start.StartupInfo.hStdInput=input.h;start.StartupInfo.hStdOutput=out_write.h;start.StartupInfo.hStdError=err_write.h;
    SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,1,0,&size);
    std::vector<unsigned char> bytes(size);start.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());
    if(!InitializeProcThreadAttributeList(start.lpAttributeList,1,0,&size))throw std::runtime_error("Cannot initialize process attributes.");
    struct Attributes {LPPROC_THREAD_ATTRIBUTE_LIST p;~Attributes(){DeleteProcThreadAttributeList(p);}} attributes{start.lpAttributeList};
    HANDLE inherited[]={input.h,out_write.h,err_write.h};
    if(!UpdateProcThreadAttribute(start.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr))
        throw std::runtime_error("Cannot restrict inherited process handles.");
    std::wstring command;for(const auto& a:args){if(!command.empty())command+=L' ';command+=quote(a);}
    PROCESS_INFORMATION info{};
    if(!CreateProcessW(wide(args.front()).c_str(),command.data(),nullptr,nullptr,TRUE,
       CREATE_SUSPENDED|CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,directory.c_str(),&start.StartupInfo,&info))
        throw std::runtime_error("MAX_LAUNCH_FAILED: Windows error "+std::to_string(GetLastError()));
    Handle process{info.hProcess},thread{info.hThread};
    if(!AssignProcessToJobObject(job.h,process.h)) {
        TerminateProcess(process.h,6);WaitForSingleObject(process.h,5000);
        throw std::runtime_error("MAX_PROCESS_ERROR: Cannot contain Max Batch in a job.");
    }
    if(ResumeThread(thread.h)==DWORD(-1))throw std::runtime_error("Cannot resume Max Batch.");
    CloseHandle(out_write.h);out_write.h=nullptr;CloseHandle(err_write.h);err_write.h=nullptr;
    ProcessResult result;const auto began=std::chrono::steady_clock::now();
    while(true) {
        drain(out_read.h,result.stdout_tail,result.stdout_truncated);drain(err_read.h,result.stderr_tail,result.stderr_truncated);
        const auto state=WaitForSingleObject(process.h,10);
        if(state==WAIT_OBJECT_0)break;
        if(state==WAIT_FAILED)throw std::runtime_error("Cannot wait for Max Batch.");
        if(std::chrono::steady_clock::now()-began>=std::chrono::seconds(timeout)) {
            result.timed_out=true;TerminateJobObject(job.h,6);WaitForSingleObject(process.h,5000);break;
        }
    }
    // Descendants may hold pipes after the launcher exits; never wait for EOF.
    TerminateJobObject(job.h,6);
    drain(out_read.h,result.stdout_tail,result.stdout_truncated);drain(err_read.h,result.stderr_tail,result.stderr_truncated);
    DWORD code=6;GetExitCodeProcess(process.h,&code);result.exit_code=int(code);
    // A tail may start within a UTF-8 character; replace malformed log bytes
    // rather than letting JSON serialization invalidate a verified export.
    auto sanitize=[](std::string& value) {
        if(value.empty())return;
        int n=MultiByteToWideChar(CP_UTF8,0,value.data(),int(value.size()),nullptr,0);
        std::wstring w(n,L'\0');MultiByteToWideChar(CP_UTF8,0,value.data(),int(value.size()),w.data(),n);
        int m=WideCharToMultiByte(CP_UTF8,0,w.data(),n,nullptr,0,nullptr,nullptr);
        std::string text(m,'\0');WideCharToMultiByte(CP_UTF8,0,w.data(),n,text.data(),m,nullptr,nullptr);
        std::replace(text.begin(),text.end(),'\0','?');value=std::move(text);
    };
    sanitize(result.stdout_tail);sanitize(result.stderr_tail);return result;
}
}
