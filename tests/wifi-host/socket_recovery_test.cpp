#include <cassert>
#include <cstdint>
#include <iostream>
static int creates,closes,last_closed,next_fd=7;
static int CreateCommandSocket() { ++creates;return next_fd; }
// Remap the extracted source's POSIX close() only inside this host harness.
// MinGW UCRT declares close() in its CRT headers; a static mock conflicts.
static int host_close(int fd) { ++closes;last_closed=fd;return 0; }
#define close host_close
#define ESP_LOGI(...) ((void)0)
#include "socket_recovery.inc"
#undef close
int main() {
    int fd=-1;uint32_t generation=0;
    assert(RefreshCommandSocketForLink(fd,generation,1) && fd==7 && generation==1);
    assert(RefreshCommandSocketForLink(fd,generation,1) && creates==1 && closes==0);
    next_fd=-1;
    assert(!RefreshCommandSocketForLink(fd,generation,2) && fd==-1 && closes==1 && last_closed==7);
    next_fd=8;
    assert(RefreshCommandSocketForLink(fd,generation,2) && fd==8 && generation==2 && closes==1);
    next_fd=9;
    assert(RefreshCommandSocketForLink(fd,generation,3) && fd==9 && generation==3 && last_closed==8);
    std::cout<<"UDP_LINK_RECOVERY=PASS unchanged-link reuse / reconnect rebind / allocation failure retry\n";
}
