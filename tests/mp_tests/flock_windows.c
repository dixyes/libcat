
/*
    The test info block is for mp_tester.php to check if the result is good.
    the mark below is for tester to find the json below.
    here is TEST_INFO_BLOCK
    {
        "marks": [
            "all nb lock done, count",
            "thread pool still usable"
        ],
        "timeout": 10
    }
*/

// manully build
// cl /I ..\deps\libuv\include /I ..\deps\libuv\inlcude\uv /I ..\include ..\build\Debug\cat.lib /MDd kernel32.lib ws2_32.lib user32.lib advapi32.lib iphlpapi.lib psapi.lib userenv.lib flock_windows.c
// manully run
// flock_windows.exe

#include "cat.h"
#include "cat_fs.h"

#define FLOCK_TEST_THREADS 24

int parent(int argc, wchar_t**argvw, HANDLE h);
int child(int argc, wchar_t**argvw, HANDLE h);
#define IPSNAME L"FlockSync"
#define FILENAME "lockfile.txt"
int wmain(int argc, wchar_t**argvw){
    int is_child = 0;
    if(argc > 1){
        if(wcsncmp(argvw[1], L"child", 6) == 0){
            is_child = 1;
        }
    }
    
    if (is_child){
        HANDLE hEvent = OpenEventW(EVENT_ALL_ACCESS, FALSE, IPSNAME);
        if(hEvent == NULL){
            // failed open
            printf("child OpenEventW failed 0x%08d\n", GetLastError());
            return 1;
        }
        return child(argc, argvw, hEvent);
    }else{
        // create event
        HANDLE hEvent = CreateEventW(NULL, FALSE, FALSE, IPSNAME);
        if(NULL == hEvent){
            // failed create
            printf("parent CreateEventW failed 0x%08d\n", GetLastError());
            return 1;
        }
        if(!ResetEvent(hEvent)){
            // failed create
            printf("parent ResetEvent failed 0x%08d\n", GetLastError());
            return 1;
        }

        // create child
        wchar_t cmd[4096];
        swprintf(cmd, 4096, L"%s child", argvw[0]);
        STARTUPINFOW si = {0};
        PROCESS_INFORMATION pi = {0};

        wprintf(L"executing \"%s\"\n", cmd);
        BOOL retb = CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
        if(!retb){
            // failed create process
            printf("CreateProcessW failed 0x%08d\n", GetLastError());
            goto fail;
        }
        int ret = parent(argc, argvw, hEvent);
        retb = TerminateProcess(pi.hProcess, 0);
        if(!retb){
            // failed kill child
            printf("TerminateProcess failed 0x%08d\n", GetLastError());
            goto fail;
        }
        return ret;
        fail:
        ResetEvent(hEvent);
        CloseHandle(hEvent);
        return 1;
    }
}

int acquire_ips(HANDLE h, int is_child, DWORD mills){
    printf("%s acquire ips lock\n", is_child ? "child": "parent");

    DWORD ret = WaitForSingleObject(h, mills);
    if (ret != WAIT_OBJECT_0){
        printf("%s WaitForSingleObject failed ret %d, 0x%08x\n", is_child ? "child": "parent", ret, GetLastError());
        return 1;
    }

    printf("%s acquired ips lock\n", is_child ? "child": "parent");
    return 0;
}

int release_ips(HANDLE h, int is_child){
    printf("%s release ips lock\n", is_child ? "child": "parent");
    BOOL retb = ResetEvent(h);
    if(!retb){
        printf("%s ResetEvent failed ret 0x%08x\n", is_child ? "child": "parent", GetLastError());
        return 1;
    }
    retb = SetEvent(h);
    if(!retb){
        printf("%s SetEvent failed ret 0x%08x\n", is_child ? "child": "parent", GetLastError());
        return 1;
    }
    printf("%s released ips lock\n", is_child ? "child": "parent");
    return 0;
}

static int locks = 0;

typedef struct flock_s {
    int fd;
} flock_t;
void* fill_thread_pools(flock_t* data){
    // this function will call cat_fs_flock to try fill thread pools.
    int ret = cat_fs_flock(data->fd, CAT_LOCK_EX | CAT_LOCK_NB);
    if(ret == 0){
        printf("flock success, this is impossible");
        abort();
    };
    // if lock nb done, set count
    locks++;
    cat_fs_flock(data->fd, CAT_LOCK_EX);
    return NULL;
}

cat_bool_t req_done = cat_false;
void* access_request(void* _){
    // send any fs request (i.e. access)
    // thread pool should be still usable if implemention is correct
    cat_fs_access(FILENAME, F_OK);
    req_done = cat_true;
    return NULL;
}

int parent(int argc, wchar_t**argvw, HANDLE h){
    printf("parent init cat\n");
    cat_init_all();
    cat_run(0);

    printf("parent unlink file %s\n", FILENAME);
    cat_fs_unlink(FILENAME);
    printf("parent open file %s\n", FILENAME);
    int fd = cat_fs_open(FILENAME, CAT_FS_O_CREAT | CAT_FS_O_RDWR, 0666);
    if(fd < 0){
        // we failed open target file
        printf("parent failed open file %s: %s\n", FILENAME, cat_get_last_error_message());
        return 1;
    }
    
    // release ips to let child start
    if(release_ips(h, 0)){
        return 1;
    }

    // wait 1s for child locks the file
    cat_time_delay(1000);

    // wait for ips
    while(0 != acquire_ips(h, 0, INFINITE)){
        return 1;
    }

    // try fill thread pool
    printf("try lock file to fill thread pool\n");

    flock_t data = {
        .fd = fd
    };
    for(int i=0; i < FLOCK_TEST_THREADS; i++){
        cat_coroutine_run(NULL, fill_thread_pools, &data);
    }

    // try do any fs operation using thread pool
    req_done = cat_false;
    cat_coroutine_run(NULL, access_request, NULL);

    int waittime;
    // sleep max 1.024s for access done
    for(waittime = 1; (!req_done) && (waittime < 1024); waittime*=2){
        cat_time_wait(waittime);
    }
    if(!req_done){
        printf("thread pool full, dead lock occoured, please report a bug\n");
        abort();
    };
    printf("thread pool still usable\n");

    // sleep for all nb lock done
    while(locks < FLOCK_TEST_THREADS){
        if(waittime < 1024){
            waittime*=2;
        }
        cat_time_wait(waittime);
    }
    printf("all nb lock done, count %d\n", locks);

    cat_fs_unlink(FILENAME);

    return 0;
}

int child(int argc, wchar_t**argvw, HANDLE h){
    printf("child init cat\n");
    cat_init_all();
    cat_run(0);

    if(acquire_ips(h, 1, 1000)){
        return 1;
    }

    printf("child open %s\n", FILENAME);
    int fd = cat_fs_open(FILENAME, CAT_FS_O_CREAT | CAT_FS_O_RDWR, 0666);
    if(fd < 0){
        // we failed open target file
        printf("child failed open file %s: %s\n", FILENAME, cat_get_last_error_message());
        return 1;
    }

    printf("child lock file\n");
    cat_fs_flock(fd, CAT_LOCK_EX);
    if(release_ips(h, 1)){
        return 1;
    }

    // wait 10s for parent to test
    cat_time_delay(10000);

    if(acquire_ips(h, 1, INFINITE)){
        return 1;
    }
    printf("child unlock file\n");
    cat_fs_flock(fd, CAT_LOCK_UN);
    if(release_ips(h, 1)){
        return 1;
    }
    // exit
    return 0;
}