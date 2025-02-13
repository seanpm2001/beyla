#include "vmlinux.h"
#include "bpf_helpers.h"
#include "bpf_tracing.h"
#include "bpf_dbg.h"
#include "pid.h"
#include "sockaddr.h"
#include "tcp_info.h"
#include "http_sock.h"
#include "http_ssl.h"

char __license[] SEC("license") = "Dual MIT/GPL";

// Temporary tracking of accept arguments
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, MAX_CONCURRENT_REQUESTS);
    __type(key, u64);
    __type(value, sock_args_t);
} active_accept_args SEC(".maps");

// Temporary tracking of connect arguments
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, MAX_CONCURRENT_REQUESTS);
    __type(key, u64);
    __type(value, sock_args_t);
} active_connect_args SEC(".maps");

// Temporary tracking of tcp_recvmsg arguments
typedef struct recv_args {
    u64 sock_ptr; // linux sock or socket address
    u64 iovec_ptr;
} recv_args_t;

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, MAX_CONCURRENT_REQUESTS);
    __type(key, u64);
    __type(value, recv_args_t);
} active_recv_args SEC(".maps");

typedef struct send_args {
    pid_connection_info_t p_conn;
    u64 size;
} send_args_t;

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, MAX_CONCURRENT_REQUESTS);
    __type(key, u64); // pid_tid
    __type(value, send_args_t); // size to be sent
} active_send_args SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, MAX_CONCURRENT_REQUESTS);
    __type(key, u64); // *sock
    __type(value, send_args_t); // size to be sent
} active_send_sock_args SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __type(key, partial_connection_info_t); // key: the connection info without the destination address, but with the tcp sequence
    __type(value, connection_info_t);  // value: traceparent info
    __uint(max_entries, 1024);
    __uint(pinning, LIBBPF_PIN_BY_NAME);
} tcp_connection_map SEC(".maps");

// Used by accept to grab the sock details
SEC("kretprobe/sock_alloc")
int BPF_KRETPROBE(kretprobe_sock_alloc, struct socket *sock) {
    u64 id = bpf_get_current_pid_tgid();

    if (!valid_pid(id)) {
        return 0;
    }

    bpf_dbg_printk("=== sock alloc %llx ===", id);

    u64 addr = (u64)sock;

    sock_args_t args = {};

    args.addr = addr;
    args.accept_time = bpf_ktime_get_ns();

    // The socket->sock is not valid until accept finishes, therefore
    // we don't extract ->sock here, we remember the address of socket
    // and parse in sys_accept
    bpf_map_update_elem(&active_accept_args, &id, &args, BPF_ANY);

    return 0;
}

// We tap into accept and connect to figure out if a request is inbound or
// outbound. However, in some cases servers can optimise the accept path if
// the same request is sent over and over. For that reason, in case we miss the
// initial accept, we establish an active filtered connection here. By default
// sets the type to be server HTTP, in client mode we'll overwrite the 
// data in the map, since those cannot be optimised.
SEC("kprobe/tcp_rcv_established")
int BPF_KPROBE(kprobe_tcp_rcv_established, struct sock *sk, struct sk_buff *skb) {
    u64 id = bpf_get_current_pid_tgid();

    if (!valid_pid(id)) {
        return 0;
    }

    bpf_dbg_printk("=== tcp_rcv_established id=%d ===", id);

    ssl_pid_connection_info_t pid_info = {};

    if (parse_sock_info(sk, &pid_info.p_conn.conn)) {
        //u16 orig_dport = info.conn.d_port;
        //dbg_print_http_connection_info(&info.conn);
        sort_connection_info(&pid_info.p_conn.conn);
        pid_info.p_conn.pid = pid_from_pid_tgid(id);        

        // This is a current limitation for port ordering detection for SSL.
        // tcp_rcv_established flip flops the ports and we can't tell if it's client or server call.
        // If the source port for a client call is lower, we'll get this wrong.
        // TODO: Need to fix this. 
        pid_info.orig_dport = pid_info.p_conn.conn.s_port,
        bpf_map_update_elem(&pid_tid_to_conn, &id, &pid_info, BPF_ANY); // to support SSL on missing handshake, respect the original info if there
    }

    return 0;
}

// We tap into both sys_accept and sys_accept4.
// We don't care about the accept entry arguments, since we get only peer information
// we don't have the full picture for the socket.
// 
// Note: A current limitation is that likely we won't capture the first accept request. The
// process may have already reached accept, before the instrumenter has launched.
SEC("kretprobe/sys_accept4")
int BPF_KRETPROBE(kretprobe_sys_accept4, uint fd)
{
    u64 id = bpf_get_current_pid_tgid();

    if (!valid_pid(id)) {
        return 0;
    }

    //bpf_dbg_printk("=== accept 4 ret id=%d ===", id);

    // The file descriptor is the value returned from the accept4 syscall.
    // If we got a negative file descriptor we don't have a connection
    if ((int)fd < 0) {
        goto cleanup;
    }

    sock_args_t *args = bpf_map_lookup_elem(&active_accept_args, &id);
    if (!args) {
        //bpf_dbg_printk("No sock info %d", id);
        goto cleanup;
    }

    bpf_dbg_printk("=== accept 4 ret id=%d, sock=%llx, fd=%d ===", id, args->addr, fd);

    ssl_pid_connection_info_t info = {};

    if (parse_accept_socket_info(args, &info.p_conn.conn)) {
        u16 orig_dport = info.p_conn.conn.d_port;
        //dbg_print_http_connection_info(&info.conn);
        sort_connection_info(&info.p_conn.conn);
        info.p_conn.pid = pid_from_pid_tgid(id);
        info.orig_dport = orig_dport;
        
        bpf_map_update_elem(&pid_tid_to_conn, &id, &info, BPF_ANY); // to support SSL on missing handshake
    }

cleanup:
    bpf_map_delete_elem(&active_accept_args, &id);
    return 0;
}

// Used by connect so that we can grab the sock details
SEC("kprobe/tcp_connect")
int BPF_KPROBE(kprobe_tcp_connect, struct sock *sk) {
    u64 id = bpf_get_current_pid_tgid();

    if (!valid_pid(id)) {
        return 0;
    }

    bpf_dbg_printk("=== tcp connect %llx ===", id);

    u64 addr = (u64)sk;

    sock_args_t args = {};

    args.addr = addr;
    args.accept_time = bpf_ktime_get_ns();

    bpf_map_update_elem(&active_connect_args, &id, &args, BPF_ANY);

    return 0;
}

// We tap into sys_connect so we can track properly the processes doing
// HTTP client calls
SEC("kretprobe/sys_connect")
int BPF_KRETPROBE(kretprobe_sys_connect, int fd)
{
    u64 id = bpf_get_current_pid_tgid();

    if (!valid_pid(id)) {
        return 0;
    }

    bpf_dbg_printk("=== connect ret id=%d, pid=%d ===", id, pid_from_pid_tgid(id));

    // The file descriptor is the value returned from the connect syscall.
    // If we got a negative file descriptor we don't have a connection, unless we are in progress
    if (fd < 0 && (fd != -EINPROGRESS)) {
        goto cleanup;
    }

    sock_args_t *args = bpf_map_lookup_elem(&active_connect_args, &id);
    if (!args) {
        bpf_dbg_printk("No sock info %d", id);
        goto cleanup;
    }

    ssl_pid_connection_info_t info = {};

    if (parse_connect_sock_info(args, &info.p_conn.conn)) {
        bpf_dbg_printk("=== connect ret id=%d, pid=%d ===", id, pid_from_pid_tgid(id));
        u16 orig_dport = info.p_conn.conn.d_port;
        //dbg_print_http_connection_info(&info.conn);
        sort_connection_info(&info.p_conn.conn);
        info.p_conn.pid = pid_from_pid_tgid(id);
        info.orig_dport = orig_dport;

        bpf_map_update_elem(&pid_tid_to_conn, &id, &info, BPF_ANY); // to support SSL 
    }

cleanup:
    bpf_map_delete_elem(&active_connect_args, &id);
    return 0;
}

// Main HTTP read and write operations are handled with tcp_sendmsg and tcp_recvmsg 

static __always_inline void *is_ssl_connection(u64 id) {
    void *ssl = 0;
    // Checks if it's sandwitched between active SSL handshake, read or write uprobe/uretprobe
    void **s = bpf_map_lookup_elem(&active_ssl_handshakes, &id);
    if (s) {
        ssl = *s;
    } else {
        ssl_args_t *ssl_args = bpf_map_lookup_elem(&active_ssl_read_args, &id);
        if (!ssl_args) {
            ssl_args = bpf_map_lookup_elem(&active_ssl_write_args, &id);
        }
        if (ssl_args) {
            ssl = (void *)ssl_args->ssl;
        }
    }         

    return ssl;
}

static __always_inline void *is_active_ssl(pid_connection_info_t *conn) {
    return bpf_map_lookup_elem(&active_ssl_connections, conn);
}

// The size argument here will be always the total response size.
// However, the return value of tcp_sendmsg tells us how much it sent. When the
// response is large it will get chunked, so we have to use a kretprobe to
// finish the request event, otherwise we won't get accurate timings.
// The problem is that kretprobes can be skipped, otherwise we could always just
// finish the request on the return of tcp_sendmsg. Therefore for any request less
// than 1MB we just finish the request on the kprobe path.
SEC("kprobe/tcp_sendmsg")
int BPF_KPROBE(kprobe_tcp_sendmsg, struct sock *sk, struct msghdr *msg, size_t size) {
    u64 id = bpf_get_current_pid_tgid();

    if (!valid_pid(id)) {
        return 0;
    }

    bpf_dbg_printk("=== kprobe tcp_sendmsg=%d sock=%llx size %d===", id, sk, size);

    send_args_t s_args = {
        .size = size
    };

    if (parse_sock_info(sk, &s_args.p_conn.conn)) {
        u16 orig_dport = s_args.p_conn.conn.d_port;
        //dbg_print_http_connection_info(&s_args.p_conn.conn); // commented out since GitHub CI doesn't like this call
        sort_connection_info(&s_args.p_conn.conn);
        s_args.p_conn.pid = pid_from_pid_tgid(id);

        void *ssl = is_ssl_connection(id);
        if (size > 0) {
            if (!ssl) {
                void *active_ssl = is_active_ssl(&s_args.p_conn);
                if (!active_ssl) {
                    u8* buf = iovec_memory();
                    if (buf) {
                        size = read_msghdr_buf(msg, buf, size);
                        if (size) {
                            u64 sock_p = (u64)sk;
                            bpf_map_update_elem(&active_send_args, &id, &s_args, BPF_ANY);
                            bpf_map_update_elem(&active_send_sock_args, &sock_p, &s_args, BPF_ANY);

                            // Logically last for !ssl.
                            handle_buf_with_connection(ctx, &s_args.p_conn, buf, size, NO_SSL, TCP_SEND, orig_dport);
                        } else {
                            bpf_dbg_printk("can't find iovec ptr in msghdr, not tracking sendmsg");
                        }
                    }
                } else {
                    bpf_dbg_printk("tcp_sendmsg for identified SSL connection, ignoring...");
                }
            } else {
                bpf_dbg_printk("tcp_sendmsg for identified SSL connection, ignoring...");
            }
        }

        if (!ssl) {
            return 0;
        }

        bpf_dbg_printk("=== kprobe SSL tcp_sendmsg=%d sock=%llx ssl=%llx ===", id, sk, ssl);
        ssl_pid_connection_info_t *s_conn = bpf_map_lookup_elem(&ssl_to_conn, &ssl);
        if (s_conn) {
            finish_possible_delayed_tls_http_request(&s_conn->p_conn, ssl);
        }
        ssl_pid_connection_info_t ssl_conn = {
            .orig_dport = orig_dport,
        };
        bpf_memcpy(&ssl_conn.p_conn, &s_args.p_conn, sizeof(pid_connection_info_t));
        bpf_map_update_elem(&ssl_to_conn, &ssl, &ssl_conn, BPF_ANY);
    }

    return 0;
}

// This is really a fallback for the kprobe to ensure we send a large request if it was
// delayed. The code under the `if (size < KPROBES_LARGE_RESPONSE_LEN) {` block should do it
// but it's possible that the kernel sends the data in smaller chunks.
SEC("kretprobe/tcp_sendmsg")
int BPF_KRETPROBE(kretprobe_tcp_sendmsg, int sent_len) {
    u64 id = bpf_get_current_pid_tgid();

    if (!valid_pid(id)) {
        return 0;
    }

    bpf_dbg_printk("=== kretprobe tcp_sendmsg=%d sent %d===", id, sent_len);

    send_args_t *s_args = bpf_map_lookup_elem(&active_send_args, &id);
    if (s_args) {
        if (sent_len > 0) {
            update_http_sent_len(&s_args->p_conn, sent_len);
        } 
        if (sent_len < MIN_HTTP_SIZE) { // Sometimes app servers don't send close, but small responses back
            finish_possible_delayed_http_request(&s_args->p_conn);
        }
    }

    return 0;
}

static __always_inline void ensure_sent_event(u64 id, u64 *sock_p) {
    send_args_t *s_args = bpf_map_lookup_elem(&active_send_args, &id);
    if (s_args) {
        bpf_dbg_printk("Checking if we need to finish the request per thread id");
        finish_possible_delayed_http_request(&s_args->p_conn);
    }  // see if we match on another thread, but same sock *
    s_args = bpf_map_lookup_elem(&active_send_sock_args, sock_p);
    if (s_args) {
        bpf_dbg_printk("Checking if we need to finish the request per socket");
        finish_possible_delayed_http_request(&s_args->p_conn);
    }
}

SEC("kprobe/tcp_close")
int BPF_KPROBE(kprobe_tcp_close, struct sock *sk, long timeout) {
    u64 id = bpf_get_current_pid_tgid();

    if (!valid_pid(id)) {
        return 0;
    }

    u64 sock_p = (u64)sk;

    bpf_dbg_printk("=== kprobe tcp_close %d sock %llx ===", id, sk);

    ensure_sent_event(id, &sock_p);

    pid_connection_info_t info = {};

    if (parse_sock_info(sk, &info.conn)) {
        sort_connection_info(&info.conn);
        //dbg_print_http_connection_info(&info.conn);
        info.pid = pid_from_pid_tgid(id);
        bpf_map_delete_elem(&ongoing_tcp_req, &info);
    }

    bpf_map_delete_elem(&active_send_args, &id);
    bpf_map_delete_elem(&active_send_sock_args, &sock_p);

    return 0;
}

//int tcp_recvmsg(struct sock *sk, struct msghdr *msg, size_t len, int flags, int *addr_len)
SEC("kprobe/tcp_recvmsg")
int BPF_KPROBE(kprobe_tcp_recvmsg, struct sock *sk, struct msghdr *msg, size_t len, int flags, int *addr_len) {
    u64 id = bpf_get_current_pid_tgid();

    if (!valid_pid(id)) {
        return 0;
    }

    bpf_dbg_printk("=== tcp_recvmsg id=%d sock=%llx ===", id, sk);

    // Make sure we don't have stale event from earlier socket connection if they are
    // sent through the same socket. This mainly happens if the server overlays virtual
    // threads in the runtime.
    u64 sock_p = (u64)sk;
    ensure_sent_event(id, &sock_p);

    // Important: We must work here to remember the iovec pointer, since the msghdr structure
    // can get modified in non-reversible way if the incoming packet is large and broken down in parts. 
    recv_args_t args = {
        .sock_ptr = (u64)sk,
        .iovec_ptr = (u64)(msg)
    };

    bpf_map_update_elem(&active_recv_args, &id, &args, BPF_ANY);

    return 0;
}

SEC("kretprobe/tcp_recvmsg")
int BPF_KRETPROBE(kretprobe_tcp_recvmsg, int copied_len) {
    u64 id = bpf_get_current_pid_tgid();

    if (!valid_pid(id)) {
        return 0;
    }

    recv_args_t *args = bpf_map_lookup_elem(&active_recv_args, &id);

    if (!args || (copied_len <= 0)) {
        bpf_map_delete_elem(&active_recv_args, &id);
        goto done;
    }

    bpf_dbg_printk("=== tcp_recvmsg ret id=%d sock=%llx copied_len %d ===", id, args->sock_ptr, copied_len);

    if (!args->iovec_ptr) {
        bpf_dbg_printk("iovec_ptr found in kprobe is NULL, ignoring this tcp_recvmsg");
        bpf_map_delete_elem(&active_recv_args, &id);

        goto done;
    }

    pid_connection_info_t info = {};

    void *iovec_ptr = (void *)args->iovec_ptr;
    void *sock_ptr = (void *)args->sock_ptr;

    bpf_map_delete_elem(&active_recv_args, &id);

    if (parse_sock_info((struct sock *)sock_ptr, &info.conn)) {
        u16 orig_dport = info.conn.d_port;
        //dbg_print_http_connection_info(&info.conn);
        sort_connection_info(&info.conn);
        info.pid = pid_from_pid_tgid(id);

        void *ssl = is_ssl_connection(id);

        if (!ssl) {
            void *active_ssl = is_active_ssl(&info);
            if (!active_ssl) {
                u8* buf = iovec_memory();
                if (buf) {
                    copied_len = read_msghdr_buf((void *)iovec_ptr, buf, copied_len);
                    if (copied_len) {
                        // doesn't return must be logically last statement
                        handle_buf_with_connection(ctx, &info, buf, copied_len, NO_SSL, TCP_RECV, orig_dport);
                    } else {
                        bpf_dbg_printk("Not copied anything");
                    }
                }
            } else {
                bpf_dbg_printk("tcp_recvmsg for an identified SSL connection, ignoring...");
            }
        } else {
            bpf_dbg_printk("tcp_recvmsg for an identified SSL connection, ignoring...");
        }
    }

done:
    return 0;
}

// Fall-back in case we don't see kretprobe on tcp_recvmsg in high network volume situations
SEC("socket/http_filter")
int socket__http_filter(struct __sk_buff *skb) {
    protocol_info_t tcp = {};
    connection_info_t conn = {};

    if (!read_sk_buff(skb, &tcp, &conn)) {
        return 0;
    }

    // ignore ACK packets
    if (tcp_ack(&tcp)) {
        return 0;
    }

    // ignore empty packets, unless it's TCP FIN or TCP RST
    if (!tcp_close(&tcp) && tcp_empty(&tcp, skb)) {
        return 0;
    }
    
    // we don't want to read the whole buffer for every packed that passes our checks, we read only a bit and check if it's truly HTTP request/response.
    unsigned char buf[MIN_HTTP_SIZE] = {0};
    bpf_skb_load_bytes(skb, tcp.hdr_len, (void *)buf, sizeof(buf));
    // technically the read should be reversed, but eBPF verifier complains on read with variable length
    u32 len = skb->len - tcp.hdr_len;
    if (len > MIN_HTTP_SIZE) {
        len = MIN_HTTP_SIZE;
    }

    u8 packet_type = 0;
    if (is_http(buf, len, &packet_type)) { // we must check tcp_close second, a packet can be a close and a response      
        //dbg_print_http_connection_info(&conn); // commented out since GitHub CI doesn't like this call
        sort_connection_info(&conn);

        http_info_t info = {0};
        info.conn_info = conn;

        if (packet_type == PACKET_TYPE_REQUEST) {
            u32 full_len = skb->len - tcp.hdr_len;
            if (full_len > FULL_BUF_SIZE) {
                full_len = FULL_BUF_SIZE;
            }
            read_skb_bytes(skb, tcp.hdr_len, info.buf, full_len);
            u64 cookie = bpf_get_socket_cookie(skb);
            //bpf_dbg_printk("=== http_filter cookie = %llx, tcp_seq=%d len=%d %s ===", cookie, tcp.seq, len, buf);
            //dbg_print_http_connection_info(&conn);
            set_fallback_http_info(&info, &conn, skb->len - tcp.hdr_len);

            // The code below is looking to see if we have recorded black-box trace info on 
            // another interface. We do this for client calls, where essentially the original 
            // request may go out on one interface, but then get re-routed to another, which is
            // common with some k8s environments.
            //
            // This casting is done here to save allocating memory on a per CPU buffer, since
            // we don't need info anymore, we reuse it's space and it's much bigger than
            // partial_connection_info_t.
            partial_connection_info_t *partial = (partial_connection_info_t *)(&info);
            partial->d_port = conn.d_port;
            partial->s_port = conn.s_port;
            partial->tcp_seq = tcp.seq;
            bpf_memcpy(partial->s_addr, conn.s_addr, sizeof(partial->s_addr));

            tp_info_pid_t *trace_info = trace_info_for_connection(&conn);
            if (trace_info) {
                if (cookie) { // we have an actual socket associated
                    bpf_map_update_elem(&tcp_connection_map, partial, &conn, BPF_ANY);
                }
            } else if (!cookie) { // no actual socket for this skb, relayed to another interface
                connection_info_t *prev_conn = bpf_map_lookup_elem(&tcp_connection_map, partial);

                if (prev_conn) {
                    tp_info_pid_t *trace_info = trace_info_for_connection(prev_conn);
                    if (trace_info) {
                        if (current_immediate_epoch(trace_info->tp.ts) == current_immediate_epoch(bpf_ktime_get_ns())) {
                            //bpf_dbg_printk("Found trace info on another interface, setting it up for this connection");
                            tp_info_pid_t other_info = {0};
                            bpf_memcpy(&other_info, trace_info, sizeof(tp_info_pid_t));
                            bpf_map_update_elem(&trace_map, &conn, &other_info, BPF_ANY);
                        }
                    }
                }
            }
        }
    }

    return 0;
}

/*
    The tracking of the clones is complicated by the fact that in container environments
    the tid returned by the sys_clone call is the namespaced tid, not the host tid which 
    bpf sees normally. To mitigate this we work exclusively with namespaces. Only the clone_map
    and server_traces are keyed off the namespace:pid.
*/
SEC("kretprobe/sys_clone")
int BPF_KRETPROBE(kretprobe_sys_clone, int tid) {
    u64 id = bpf_get_current_pid_tgid();

    if (!valid_pid(id) || tid < 0) {
        return 0;
    }

    pid_key_t parent = {0};
    task_tid(&parent);

    pid_key_t child = {
        .pid = (u32)tid,
        .ns = parent.ns,
    };

    bpf_dbg_printk("sys_clone_ret %d -> %d", id, tid);
    bpf_map_update_elem(&clone_map, &child, &parent, BPF_ANY);
    
    return 0;
}

SEC("kprobe/sys_exit")
int BPF_KPROBE(kprobe_sys_exit, int status) {
    u64 id = bpf_get_current_pid_tgid();

    if (!valid_pid(id)) {
        return 0;
    }

    trace_key_t task = {0};
    task_tid(&task.p_key);

    bpf_dbg_printk("sys_exit %d, pid=%d, valid_pid(id)=%d", id, pid_from_pid_tgid(id), valid_pid(id));
 
    // handle the case when the thread terminates without closing a socket
    send_args_t *s_args = bpf_map_lookup_elem(&active_send_args, &id);
    if (s_args) {
        bpf_dbg_printk("Checking if we need to finish the request per thread id");
        finish_possible_delayed_http_request(&s_args->p_conn);
        bpf_map_delete_elem(&active_ssl_connections, &s_args->p_conn);
    }

    bpf_map_delete_elem(&clone_map, &task.p_key);
    // This won't delete trace ids for traces with extra_id, like NodeJS. But, 
    // we expect that it doesn't matter, since NodeJS main thread won't exit. 
    bpf_map_delete_elem(&server_traces, &task);
    
    return 0;
}