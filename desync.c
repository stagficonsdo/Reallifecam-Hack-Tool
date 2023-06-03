#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/mman.h>

#ifdef __linux__
#include <sys/sendfile.h>
#else
#include <sys/uio.h>
#define sendfile(outfd, infd, start, len) sendfile(infd, outfd, start, len, 0, 0)
#endif

#ifdef MFD_CLOEXEC
#include <sys/syscall.h>
#define memfd_create(name, flags) syscall(__NR_memfd_create, name, flags);
#else
#define memfd_create(name, flags) fileno(tmpfile())
#endif

#include <params.h>
#include <packets.h>


int fake_attack(int sfd, char *buffer, ssize_t n, int cnt, int pos)
{
    struct packet pkt = cnt != IS_HTTP ? fake_tls : fake_http;
    size_t psz = pkt.size;
    
    int ffd = memfd_create("name", O_RDWR);
    if (ffd < 0) {
        perror("memfd_create");
        return -1;
    }
    char *p = 0;
    int status = -1;
    
    while (status) {
        if (ftruncate(ffd, pos) < 0) {
            perror("ftruncate");
            break;
        }
        p = mmap(0, pos, PROT_WRITE, MAP_SHARED, ffd, 0);
        if (p == MAP_FAILED) {
            perror("mmap");
            break;
        }
        memcpy(p, pkt.data, psz < pos ? psz : pos);
        
        if (setsockopt(sfd, IPPROTO_IP, IP_TTL,
                 &params.ttl, sizeof(params.ttl)) < 0) {
            perror("setsockopt IP_TTL");
            break;
        }
        if (sendfile(sfd, ffd, 0, pos) < 0) {
            perror("sendfile");
            break;
        }
        usleep(params.sfdelay);
        memcpy(p, buffer, pos);
        
        if (setsockopt(sfd, IPPROTO_IP, IP_TTL,
                 &params.def_ttl, sizeof(int)) < 0) {
            perror("setsockopt IP_TTL");
            break;
        }
        if (send(sfd, buffer + pos, n - pos, 0) < 0) {
            perror("send");
            break;
        }
        status = 0;
    }
    if (p) munmap(p, pos);
    close(ffd);
    return status;
}


int disorder_attack(int sfd, char *buffer, ssize_t n, int pos)
{
    int bttl = 1;
    if (setsockopt(sfd, IPPROTO_IP, IP_TTL,
             (char *)&bttl, sizeof(bttl)) < 0) {
        perror("setsockopt IP_TTL");
        return -1;
    }
    if (send(sfd, buffer, pos, 0) < 0) {
        perror("send");
        return -1;
    }
    if (setsockopt(sfd, IPPROTO_IP, IP_TTL,
            (char *)&params.def_ttl, sizeof(int)) < 0) {
        perror("setsockopt IP_TTL");
        return -1;
    }
    if (send(sfd, buffer + pos, n - pos, 0) < 0) {
        perror("send");
        return -1;
    }
    return 0;
}


int desync(int sfd, char *buffer, ssize_t n)
{
    int pos = params.split;
    char *host = 0;
    int len = 0, type = 0;
    
    if ((len = parse_tls(buffer, n, &host))) {
        type = IS_HTTPS;
    }
    else if ((len = parse_http(buffer, n, &host, 0))) {
        type = IS_HTTP;
    }
    LOG(LOG_S, "host: %.*s\n", len, host);
    
    if (type == IS_HTTP && params.mod_http) {
        if (mod_http(buffer, n, params.mod_http)) {
            fprintf(stderr, "mod http error\n");
            return -1;
        }
    }
    if (host && params.split_host)
        pos += (host - buffer);
    else if (pos < 0)
        pos += n;
    
    LOG(LOG_L, "split pos: %d, n: %ld\n", pos, n);
    
    if (pos <= 0 || pos >= n ||
            params.attack == DESYNC_NONE ||
            (!type && params.de_known)) 
    {
        if (send(sfd, buffer, n, 0) < 0) {
            perror("send");
            return -1;
        }
    }
    else switch (params.attack) {
        case DESYNC_FAKE:
            return fake_attack(sfd, buffer, n, type, pos);
            
        case DESYNC_DISORDER:
            return disorder_attack(sfd, buffer, n, pos);
        
        case DESYNC_SPLIT:
        default:
            if (send(sfd, buffer, pos, 0) < 0) {
                perror("send");
                return -1;
            }
            if (send(sfd, buffer + pos, n - pos, 0) < 0) {
                perror("send");
                return -1;
            }
    }
    return 0;
}