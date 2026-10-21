/* Real broken-stream writes through production network send paths. */
#if defined(TEST_SYS_NET)
#define main existing_sys_net_test_main
#include "test_sys_net.c"
#undef main
#elif defined(TEST_HTTP)
#include "../cellHttp.c"
uint8_t* vm_base;
#elif defined(TEST_PSNR)
#include "../psnr/psnr.c"
#else
#error Select a production send path
#endif

#include <assert.h>
#include <signal.h>
#include <string.h>

static volatile sig_atomic_t signals_seen;
static void custom_sigpipe(int signal_number) { (void)signal_number; ++signals_seen; }

int main(int argc, char** argv)
{
    assert(argc == 3);
    const int custom = !strcmp(argv[2], "custom");
    const int healthy = !strcmp(argv[2], "success");
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = custom ? custom_sigpipe : SIG_DFL;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGPIPE, &action, NULL) == 0);
    sigset_t before, after;
    assert(sigprocmask(SIG_SETMASK, NULL, &before) == 0);
    int pair[2];
#if defined(TEST_SYS_NET)
    /* An AF_INET stream makes explicit sendto's destination valid on every
     * host, rather than passing an IPv4 address to an AF_UNIX socket. */
    struct sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET; peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int listener = socket(AF_INET, SOCK_STREAM, 0); assert(listener >= 0);
    assert(bind(listener, (struct sockaddr*)&peer, sizeof(peer)) == 0);
    assert(listen(listener, 1) == 0);
    socklen_t peer_len = sizeof(peer);
    assert(getsockname(listener, (struct sockaddr*)&peer, &peer_len) == 0);
    pair[0] = socket(AF_INET, SOCK_STREAM, 0); assert(pair[0] >= 0);
    assert(connect(pair[0], (struct sockaddr*)&peer, sizeof(peer)) == 0);
    pair[1] = accept(listener, NULL, NULL); assert(pair[1] >= 0);
    close(listener);
#else
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
#endif
    if (!healthy) assert(shutdown(pair[0], SHUT_WR) == 0); /* Deterministic EPIPE. */
    if (!strcmp(argv[1], "raw")) {
        send(pair[0], "x", 1, 0);
        return 99; /* Default SIGPIPE must terminate this negative control. */
    }
#if defined(TEST_SYS_NET)
    vm_base = (uint8_t*)calloc(1, 1u << 20); assert(vm_base);
    ps3_net_host_register(bump_alloc);
    int slot = alloc_slot(pair[0]); assert(slot > 0);
    memcpy(vm_base + BUF, "x", 1);
    int32_t result;
    if (!strcmp(argv[1], "send"))
        result = call("send", slot, BUF, 1, 0, 0, 0);
    else if (!strcmp(argv[1], "sendto-null"))
        result = call("sendto", slot, BUF, 1, 0, 0, 0);
    else {
        put_sockaddr(ADDR_A, 0x7F000001u, ntohs(peer.sin_port));
        result = call("sendto", slot, BUF, 1, 0, ADDR_A, 16);
    }
    if (healthy) assert(result == 1);
    else {
        assert((uint32_t)result == 0x80010220u);
        assert(errno_cell() == 32);
    }
#elif defined(TEST_HTTP)
    assert(http_send_all(pair[0], "x", 1) == (healthy ? 0 : -1));
    if (!healthy) assert(errno == EPIPE);
#elif defined(TEST_PSNR)
    psnr_client client; memset(&client, 0, sizeof(client)); client.s = pair[0];
    assert(send_all(&client, (const uint8_t*)"x", 1) == (healthy ? 0 : -1));
    assert(client.dead == !healthy);
    if (!healthy) assert(errno == EPIPE);
#endif
    if (healthy) { char received = 0; assert(recv(pair[1], &received, 1, 0) == 1); assert(received == 'x'); }
    assert(sigaction(SIGPIPE, NULL, &action) == 0);
    assert(action.sa_handler == (custom ? custom_sigpipe : SIG_DFL));
    assert(sigprocmask(SIG_SETMASK, NULL, &after) == 0);
    for (int i = 1; i < NSIG; ++i)
        assert(sigismember(&before, i) == sigismember(&after, i));
    assert(signals_seen == 0);
    if (custom) { assert(raise(SIGPIPE) == 0); assert(signals_seen == 1); }
    close(pair[0]); close(pair[1]);
    puts("broken-stream send returns EPIPE with signal disposition/mask intact");
    return 0;
}
