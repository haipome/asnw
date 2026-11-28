/*
 * Description: address type, address parsing and socket option helpers
 *     History: yang@haipo.me, 2016/03/16, create
 */

# ifndef _NW_SOCK_H_
# define _NW_SOCK_H_

# include <stdint.h>
# include <sys/types.h>
# include <sys/resource.h>
# include <sys/stat.h>
# include <sys/socket.h>
# include <sys/un.h>
# include <arpa/inet.h>
# include <netinet/in.h>
# include <netinet/tcp.h>

/* the family of a nw_addr that holds a host name. no AF_ value is that big */
# define NW_SOCK_AF_NAME 0x10000
/* room for a host name in a nw_addr, '\0' included. it shares the place of
 * the unix path: a nw_addr is no bigger for it */
# define NW_SOCK_MAX_NAME (sizeof(struct sockaddr_un) - sizeof(uint16_t))

/*
 * nw_addr is one address of any supported kind: ipv4, ipv6 or unix path.
 *
 * or the name of a host, with a port, in place of an ipv4 or ipv6 address.
 * a name is not an address yet: it is for a nw_clt, which looks it up
 * each time it connects. nothing else takes one, a server can't listen on
 * a name.
 */
typedef struct nw_addr {
    /* AF_INET, AF_INET6, AF_UNIX or NW_SOCK_AF_NAME: tells which member of
     * the union is used */
    unsigned int family;
    /* length of the address, as passed to bind / connect / sendto. 0 for
     * a name */
    unsigned int addrlen;
    union {
        struct sockaddr_in  in;
        struct sockaddr_in6 in6;
        struct sockaddr_un  un;
        struct {
            /* in host byte order */
            uint16_t port;
            char     name[NW_SOCK_MAX_NAME];
        } host;
    };
} nw_addr;

/* the struct sockaddr pointer of a nw_addr pointer, for the socket calls */
# define NW_SOCKADDR(addr) ((struct sockaddr *)(&(addr)->in))
/* buffer size for nw_sock_human_addr_s */
# define NW_SOCK_HUMAN_ADDR_SIZE 128
/* buffer size for nw_sock_ip_s */
# define NW_SOCK_IP_SIZE    INET6_ADDRSTRLEN

/*
 * format an address for logs:
 *   ipv4: 127.0.0.1:3333
 *   ipv6: [::1]:3333
 *   unix: unix:/tmp/test.sock
 *   name: db.local:3333
 * returns a static buffer (one per thread) that the next call overwrites,
 * so don't use it twice in one printf and don't keep the pointer
 */
char *nw_sock_human_addr(nw_addr *addr);

/* same, written to dest, which must hold at least NW_SOCK_HUMAN_ADDR_SIZE bytes.
 * returns dest */
char *nw_sock_human_addr_s(nw_addr *addr, char *dest);

/* the ip of an ipv4 / ipv6 address without the port, an empty string for a
 * unix address or a name. returns a static buffer, like nw_sock_human_addr */
char *nw_sock_ip(nw_addr *addr);

/* same, written to ip, which must hold at least NW_SOCK_IP_SIZE bytes.
 * returns ip */
char *nw_sock_ip_s(nw_addr *addr, char *ip);

/* chmod the socket file of a unix address. does nothing and returns 0 for
 * other addresses. return 0 on success, -1 on error */
int nw_sock_set_mode(nw_addr *addr, mode_t mode);

/*
 * parse an address written as protocol@address into addr and sock_type.
 *
 *   protocol    address             sock_type
 *   tcp         ip:port, name:port  SOCK_STREAM
 *   udp         ip:port, name:port  SOCK_DGRAM
 *   stream      unix socket path    SOCK_STREAM
 *   dgram       unix socket path    SOCK_DGRAM
 *   seqpacket   unix socket path    SOCK_SEQPACKET
 *
 * the protocol is case insensitive. ipv6 is written [ip]:port (a bare
 * ip:port is accepted too, the port is what follows the last colon). the
 * port is decimal, 0 to 65535.
 *
 * what stands in the place of the ip and is none is taken for the name of
 * a host: letters, digits, '-', '_' and '.', less than NW_SOCK_MAX_NAME
 * of them. nothing is looked up here, addr just holds the name and the
 * port (family NW_SOCK_AF_NAME), for a nw_clt.
 *
 * example: tcp@127.0.0.1:3333
 *          tcp@[::1]:3333
 *          tcp@db.local:3333
 *          dgram@/tmp/test.sock
 *
 * return 0 on success, or
 *   -1: no '@'
 *   -2: unknown protocol
 *   -3: no port
 *   -4: bad ip or port
 *   -5: unix path empty or too long
 *   -6: neither an ip nor a possible name
 */
int nw_sock_addr_parse(const char *str, nw_addr *addr, int *sock_type);

/* the functions below return 0 on success and -1 on error with errno set,
 * except nw_sock_errno */

/* get the address of the other end of a connected socket */
int nw_sock_peer_addr(int sockfd, nw_addr *addr);

/* get the local address of a socket, e.g. the port picked for a bind to port 0 */
int nw_sock_local_addr(int sockfd, nw_addr *addr);

/* fetch and clear the pending error of a socket (SO_ERROR): 0 if there is
 * none, otherwise an errno value. tells how a non blocking connect ended */
int nw_sock_errno(int sockfd);

/* get the kernel send buffer size of a socket (SO_SNDBUF) */
int nw_sock_get_send_buf(int sockfd, int *buf_size);

/* get the kernel receive buffer size of a socket (SO_RCVBUF) */
int nw_sock_get_recv_buf(int sockfd, int *buf_size);

/* set the kernel send buffer size of a socket. the kernel may round or
 * limit the value, read it back to know what was applied */
int nw_sock_set_send_buf(int sockfd, int buf_size);

/* set the kernel receive buffer size of a socket, same remark */
int nw_sock_set_recv_buf(int sockfd, int buf_size);

/* make a socket non blocking */
int nw_sock_set_nonblock(int sockfd);

/* have a socket closed when the process executes another program, so
 * that it is not handed down to it. every socket the library creates or
 * accepts is like that */
int nw_sock_set_cloexec(int sockfd);

/* disable the Nagle algorithm of a tcp socket (TCP_NODELAY) */
int nw_sock_set_no_delay(int sockfd);

/* allow binding an address still in TIME_WAIT (SO_REUSEADDR) */
int nw_sock_set_reuse_addr(int sockfd);

# endif
