#include <linux/file.h>
#include <linux/in.h>
#include <linux/init.h>
#include <linux/ipv6.h>
#include <linux/kallsyms.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <linux/syscalls.h>
#include <linux/version.h>
#include <linux/unistd.h>
#include <net/ip.h>
#include "whitelist.h"
#include "netlog.h"
#include "log.h"
#include "probes.h"
#include "sparse_compat.h"
#include "retro-compat.h"
#include "internal.h"
#include "../lib/probes_helper.h"

/********************************/
/*          Variables           */
/********************************/

static u8 initialized;
static u32 loaded_probes;
static DEFINE_SPINLOCK(probe_lock);

struct probes probe_list[] = {
        { "tcp_connect", 1 << PROBE_TCP_CONNECT },
        { "tcp_accept",  1 << PROBE_TCP_ACCEPT},
        { "tcp_close",   1 << PROBE_TCP_CLOSE},
        { "udp_connect", 1 << PROBE_UDP_CONNECT},
        { "udp_bind",    1 << PROBE_UDP_BIND},
        { "udp_close",   1 << PROBE_UDP_CLOSE},
};

#ifdef CONFIG_X86
#ifdef CONFIG_X86_64
/* Calling conventions: RDI, RSI, RDX */
#define GET_ARG_1(regs) regs->di
#define GET_ARG_2(regs) regs->si
#define GET_ARG_3(regs) regs->dx
#else /* !CONFIG_X86_64 */
/* Calling conventions: AX, DX, BX */
#define GET_ARG_1(regs) regs->ax
#define GET_ARG_2(regs) regs->dx
#define GET_ARG_3(regs) regs->bx
#endif /* CONFIG_X86_64 ? */
#else
#error Unsupported architecture
#endif

/********************************/
/*            Tools             */
/********************************/

static char *path_from_mm(struct mm_struct *mm, char *buffer, int length)
{
	char *p = NULL;
	if (unlikely(mm == NULL))
		return NULL;

	down_read(&mm->mmap_sem);

	if (unlikely(mm->exe_file == NULL)) {
		p = NULL;
	} else {
		p = d_path(&mm->exe_file->f_path, buffer, length);
		if (IS_ERR(p))
			p = NULL;
	}

	up_read(&mm->mmap_sem);
	return p;
}

static void log_if_not_whitelisted(struct socket *sock, u8 protocol, u8 action)
{
	/* sock & sock->sk need to be non null */

	char buffer[MAX_EXEC_PATH + 1], *path;
	unsigned short family;
	const void *dst_ip;
	const void *src_ip;
	int dst_port;
	int src_port;

	path = path_from_mm(current->mm, buffer, MAX_EXEC_PATH);
	buffer[MAX_EXEC_PATH] = '\0';
	if (unlikely(path == NULL))
		return;

	/* Get everything */
	family = sock->sk->sk_family;
	dst_port = ntohs(inet_sk(sock->sk)->DPORT);
	src_port = ntohs(inet_sk(sock->sk)->SPORT);
	switch (family) {
	case AF_INET:
		dst_ip = &inet_sk(sock->sk)->DADDR;
		src_ip = &inet_sk(sock->sk)->SADDR;
		break;
	case AF_INET6:
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 13, 0)
		dst_ip = &sock->sk->sk_v6_daddr;
#else /* LINUX_VERSION_CODE < KERNEL_VERSION(3, 13, 0) */
		dst_ip = &inet6_sk(sock->sk)->daddr;
#endif /* LINUX_VERSION_CODE ? KERNEL_VERSION(3, 13, 0) */
		src_ip = &inet6_sk(sock->sk)->saddr;
		break;
	default:
		dst_ip = NULL;
		src_ip = NULL;
		break;
	}

#if WHITELISTING
	/* Are we whitelisted ? */
	if (is_whitelisted(path, family, dst_ip, dst_port))
		return;
#endif

	store_netlog_record(path, action, protocol,
			    family, src_ip, src_port, dst_ip, dst_port);
}


/**********************************/
/*           PROBES               */
/**********************************/

struct probe_data {
	struct socket *sock;
};

static int pre_handler_store_sock(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct probe_data *priv = (struct probe_data*)ri->data;

	if (likely(current != NULL)) {
		priv->sock = (struct socket*)GET_ARG_1(regs);
		return 0;
	}
	return 1;
}

static int post_inet_stream_connect(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct probe_data *priv = (struct probe_data*)ri->data;
	struct socket *sock = priv->sock;

	if (likely(current != NULL) &&
	    likely(sock != NULL) &&
	    likely(sock->sk != NULL) &&
	    likely(sock->sk->sk_family == AF_INET ||
		   sock->sk->sk_family == AF_INET6) &&
	    likely(sock->sk->sk_protocol == IPPROTO_TCP))
		log_if_not_whitelisted(sock, PROTO_TCP, ACTION_CONNECT);

	return 0;
}

static int post_inet_dgram_connect(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct probe_data *priv = (struct probe_data*)ri->data;
	struct socket *sock = priv->sock;

	if (likely(current != NULL) &&
	    likely(sock != NULL) &&
	    likely(sock->sk != NULL) &&
	    likely(sock->sk->sk_family == AF_INET ||
		   sock->sk->sk_family == AF_INET6) &&
	    likely(sock->sk->sk_protocol == IPPROTO_UDP))
		log_if_not_whitelisted(sock, PROTO_UDP, ACTION_CONNECT);

	return 0;
}

/* post_sys_accept probe is called right after the accept system call returns.
 * In the return register is placed the socket file descriptor. So with the
 * user of regs_register_status we can get the socket file descriptor and log
 * the data that we want for the socket.
 */

static int post_sys_accept(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct socket *sock;
	int err;

	sock = sockfd_lookup(regs_return_value(regs), &err);

	if (likely(sock != NULL)) {
		if (likely(sock->sk != NULL) &&
		    likely(sock->sk->sk_family == AF_INET ||
			   sock->sk->sk_family == AF_INET6) &&
		    likely(sock->sk->sk_protocol == IPPROTO_TCP))
			log_if_not_whitelisted(sock, PROTO_TCP, ACTION_ACCEPT);
		sockfd_put(sock);
	}
	return 0;
}

asmlinkage static long pre_sys_close(unsigned int fd)
{
	struct socket *sock;
	int err;

	sock = sockfd_lookup(fd, &err);

	if (unlikely(current == NULL) ||
	    unlikely(sock == NULL) ||
	    unlikely(sock->sk == NULL) ||
	    likely(sock->sk->sk_family != AF_INET &&
		   sock->sk->sk_family != AF_INET6))
		goto out;

	if ((loaded_probes & (1 << PROBE_TCP_CLOSE)) &&
	    sock->sk->sk_protocol == IPPROTO_TCP &&
	    likely(inet_sk(sock->sk)->DPORT != 0))
		log_if_not_whitelisted(sock, PROTO_TCP, ACTION_CLOSE);
	else if ((loaded_probes & (1 << PROBE_UDP_CLOSE)) &&
		 sock->sk->sk_protocol == IPPROTO_UDP &&
		 inet_sk(sock->sk)->SPORT != 0)
		log_if_not_whitelisted(sock, PROTO_UDP, ACTION_CLOSE);

out:
	if (likely(sock != NULL))
		sockfd_put(sock);

	jprobe_return();
	return 0;
}

static int pre_sys_bind(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	int err;
	struct probe_data *priv = (struct probe_data*)ri->data;
	struct socket *sock;

	if (unlikely(current == NULL))
		return 1;

	sock = sockfd_lookup((int)GET_ARG_1(regs), &err);

	if (likely(sock != NULL)) {
		priv->sock = sock;
		return 0;
	}

	return 1;
}

static int post_sys_bind(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct probe_data *priv = (struct probe_data*)ri->data;
	struct socket *sock = priv->sock;

	if (likely(sock != NULL)) {
		if (likely(sock->sk != NULL) &&
		    likely(sock->sk->sk_family == AF_INET ||
			   sock->sk->sk_family == AF_INET6) &&
		    likely(sock->sk->sk_protocol == IPPROTO_UDP))
			log_if_not_whitelisted(sock, PROTO_UDP, ACTION_BIND);
		sockfd_put(sock);
	}

	return 0;
}

/* UDP protocol is connectionless protocol, so we probe the bind system call */

/*************************************/
/*         probe definitions        */
/*************************************/

static struct kretprobe stream_connect_kretprobe = {
	.entry_handler = pre_handler_store_sock,
	.handler = post_inet_stream_connect,
	.data_size = sizeof(struct socket*),
	.maxactive = 16 * NR_CPUS,
	.kp = {
		.symbol_name = "inet_stream_connect",
		.fault_handler = handler_fault,
	},
};

static struct kretprobe dgram_connect_kretprobe = {
	.entry_handler = pre_handler_store_sock,
	.handler = post_inet_dgram_connect,
	.data_size = sizeof(struct socket*),
	.maxactive = 16 * NR_CPUS,
	.kp = {
		.symbol_name = "inet_dgram_connect",
		.fault_handler = handler_fault,
	},
};

static struct kretprobe accept_kretprobe = {
	.handler = post_sys_accept,
	.maxactive = 16 * NR_CPUS,
	.kp = {
		#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 32)
		.symbol_name = "sys_accept",
		#else
		.symbol_name = "sys_accept4",
		#endif
		.fault_handler = handler_fault,
	},
};

static struct jprobe close_jprobe = {
	.entry = (kprobe_opcode_t *)pre_sys_close,
	.kp = {
		.symbol_name = "sys_close",
		.fault_handler = handler_fault,
	}
};

static struct kretprobe bind_kretprobe = {
	.entry_handler = pre_sys_bind,
	.handler = post_sys_bind,
	.data_size = sizeof(struct socket*),
	.maxactive = 16 * NR_CPUS,
	.kp = {
		.symbol_name = "sys_bind",
		.fault_handler = handler_fault,
	},
};


/****************************************/
/*     Planting/unplanting probes       */
/****************************************/

static void
unplant_probes(u32 removed_probes)
__must_hold(probe_lock)
{
	loaded_probes ^= removed_probes;

	if (removed_probes & (1 << PROBE_TCP_CONNECT))
		unplant_kretprobe(&stream_connect_kretprobe);

	if (removed_probes & (1 << PROBE_TCP_ACCEPT))
		unplant_kretprobe(&accept_kretprobe);

	if (removed_probes & ((1 << PROBE_TCP_CLOSE) | (1 << PROBE_UDP_CLOSE))) {
		if (!(loaded_probes & ((1 << PROBE_TCP_CLOSE) | (1 << PROBE_UDP_CLOSE))))
			unplant_jprobe(&close_jprobe);
	}
	if (removed_probes & (1 << PROBE_UDP_CONNECT))
		unplant_kretprobe(&dgram_connect_kretprobe);

	if (removed_probes & (1 << PROBE_UDP_BIND))
		unplant_kretprobe(&bind_kretprobe);
}

void unplant_all(void)
{
	unsigned long flags;

	spin_lock_irqsave(&probe_lock, flags);

	unplant_probes(loaded_probes);

	spin_unlock_irqrestore(&probe_lock, flags);
}

static int
plant_probes(u32 new_probes)
__must_hold(&probe_lock)
{
	int err = 0;

	if (new_probes & (1 << PROBE_TCP_CONNECT)) {
		err = plant_kretprobe(&stream_connect_kretprobe);
		if (err < 0)
			return -CONNECT_PROBE_FAILED;
		loaded_probes |= 1 << PROBE_TCP_CONNECT;
	}

	if (new_probes & (1 << PROBE_TCP_ACCEPT)) {
		err = plant_kretprobe(&accept_kretprobe);
		if (err < 0)
			return -ACCEPT_PROBE_FAILED;
		loaded_probes |= 1 << PROBE_TCP_ACCEPT;
	}

	if (new_probes & (1 << PROBE_TCP_CLOSE)) {
		if (!(loaded_probes & (1 << PROBE_UDP_CLOSE))) {
			err = plant_jprobe(&close_jprobe);
			if (err < 0)
				return -CLOSE_PROBE_FAILED;
		}
		loaded_probes |= 1 << PROBE_TCP_CLOSE;
	}
	if (new_probes & (1 << PROBE_UDP_CONNECT)) {
		err = plant_kretprobe(&dgram_connect_kretprobe);
		if (err)
			return -CONNECT_PROBE_FAILED;
		loaded_probes |= 1 << PROBE_UDP_CONNECT;
	}

	if (new_probes & (1 << PROBE_UDP_BIND)) {
		err = plant_kretprobe(&bind_kretprobe);
		if (err < 0)
			return -BIND_PROBE_FAILED;
		loaded_probes |= 1 << PROBE_UDP_BIND;
	}

	if (new_probes & (1 << PROBE_UDP_CLOSE)) {
		if (!(loaded_probes & (1 << PROBE_TCP_CLOSE))) {
			err = plant_jprobe(&close_jprobe);
			if (err < 0)
				return -CLOSE_PROBE_FAILED;
		}
		loaded_probes |= 1 << PROBE_UDP_CLOSE;
	}

	return err;
}

/***********************************************/
/*               ""Initializer""               */
/***********************************************/

/*
 * The following function only do something if initialized != 0
 * i.e if no parameter was set (yet)
*/

int
probes_init(void)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&probe_lock, flags);
	if (initialized != 0) {
		ret = plant_probes(DEFAULT_PROBES);
		if (ret >= 0)
			initialized = 1;
	}
	spin_unlock_irqrestore(&probe_lock, flags);

	return ret;
}

/***********************************************/
/*     GETTER/SETTER for module parameters     */
/***********************************************/

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 36)
int
all_probes_param_set(const char *buf, struct kernel_param *kp)
#else /* LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 36) */
static int
all_probes_param_set(const char *buf, const struct kernel_param *kp)
#endif /* LINUX_VERSION_CODE ? KERNEL_VERSION(2, 6, 36) */
{
	unsigned long flags;
	u32 probes_to_add;
	u32 probes_to_remove;
	unsigned long wanted_probes;
	int ret;

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 2, 0)
	ret = strict_strtoul(buf, 16, &wanted_probes);
#else /* LINUX_VERSION_CODE > KERNEL_VERSION(3, 2, 0) */
	ret = kstrtoul(buf, 16, &wanted_probes);
#endif /* LINUX_VERSION_CODE ? KERNEL_VERSION(3, 2, 0) */
	if (ret < 0)
		return ret;

	spin_lock_irqsave(&probe_lock, flags);

	if (initialized != 0) {
		ret = plant_probes(DEFAULT_PROBES);
		if (ret < 0)
			goto fail;
		initialized = 1;
	}

	probes_to_add = wanted_probes & (~loaded_probes);
	probes_to_remove = (~wanted_probes) & loaded_probes;

	unplant_probes(probes_to_remove);
	ret = plant_probes(probes_to_add);

fail:
	spin_unlock_irqrestore(&probe_lock, flags);
	return ret;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 36)
int
all_probes_param_get(char *buffer, struct kernel_param *kp)
#else /* LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 36) */
static int
all_probes_param_get(char *buffer, const struct kernel_param *kp)
#endif /* LINUX_VERSION_CODE ? KERNEL_VERSION(2, 6, 36) */
{
	unsigned long flags;
	int ret;

        spin_lock_irqsave(&probe_lock, flags);
	ret = scnprintf(buffer, PAGE_SIZE, "%x", loaded_probes);
	spin_unlock_irqrestore(&probe_lock, flags);

	return ret;
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 36)
const struct kernel_param_ops all_probes_param = {
        .set = all_probes_param_set,
        .get = all_probes_param_get,
};
#endif /* LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 36) */


#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 36)
int
one_probe_param_set(const char *buf, struct kernel_param *kp)
#else /* LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 36) */
static int
one_probe_param_set(const char *buf, const struct kernel_param *kp)
#endif /* LINUX_VERSION_CODE ? KERNEL_VERSION(2, 6, 36) */
{
	unsigned long flags;
	unsigned long value;
	int ret;
	struct probes *probe;

	probe = (struct probes*)kp->arg;
	if (unlikely(probe == NULL))
		return -EBADF;

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 2, 0)
	ret = strict_strtoul(buf, 0, &value);
#else /* LINUX_VERSION_CODE > KERNEL_VERSION(3, 2, 0) */
	ret = kstrtoul(buf, 0, &value);
#endif /* LINUX_VERSION_CODE ? KERNEL_VERSION(3, 2, 0) */
	if (ret < 0)
		return ret;
	ret = 0;

        spin_lock_irqsave(&probe_lock, flags);

	if (initialized != 0) {
		ret = plant_probes(DEFAULT_PROBES);
		if (ret < 0)
			goto fail;
		initialized = 1;
	}

	if (value) {
		if (probe->mask & (~loaded_probes))
			ret = plant_probes(probe->mask);
	} else {
		if (probe->mask &loaded_probes)
			unplant_probes(probe->mask);
	}

fail:
	spin_unlock_irqrestore(&probe_lock, flags);
	return ret;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 36)
int
one_probe_param_get(char *buffer, struct kernel_param *kp)
#else /* LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 36) */
static int
one_probe_param_get(char *buffer, const struct kernel_param *kp)
#endif /* LINUX_VERSION_CODE ? KERNEL_VERSION(2, 6, 36) */
{
	unsigned long flags;
	int ret;
	struct probes *probe;

	probe = (struct probes*)kp->arg;
	if (unlikely(probe == NULL))
		return -EBADF;

        spin_lock_irqsave(&probe_lock, flags);
	ret = scnprintf(buffer, PAGE_SIZE, "%i", !!(probe->mask & loaded_probes));
	spin_unlock_irqrestore(&probe_lock, flags);

	return ret;
}


#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 36)
const struct kernel_param_ops one_probe_param = {
        .set = one_probe_param_set,
        .get = one_probe_param_get,
};
#endif /* LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 36) */
